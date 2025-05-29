/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodOverrideGraph.h"

#include <boost/range/adaptor/map.hpp>

#include <iterator>
#include <sparta/PatriciaTreeMap.h>
#include <sparta/PatriciaTreeSet.h>

#include "BinarySerialization.h"
#include "CppUtil.h"
#include "Show.h"
#include "Timer.h"
#include "Walkers.h"

using namespace method_override_graph;

namespace {

using MethodSet = sparta::PatriciaTreeSet<const DexMethod*>;

using ProtoMap = sparta::PatriciaTreeMap<const DexProto*, MethodSet>;

// The set of methods in scope at a particular class. We use PatriciaTreeMaps
// for this because there is a lot shared structure: the maps of subclasses /
// subinterfaces contain many elements from their parent classes / interfaces.
// We also do a lot of unioning operations when analyzing interfaces, and
// PatriciaTreeMaps are well-optimized for that.
using SignatureMap =
    sparta::PatriciaTreeMap<const DexString* /* name */, ProtoMap>;

struct ClassSignatureMap {
  // The methods implemented by the current class or one of its superclasses.
  // The MethodSets here should always be singleton sets.
  SignatureMap implemented;
  // The interface methods not yet implemented by the current class or its
  // superclasses.
  // The MethodSets here can have multiple elements -- a class can implement
  // multiple interfaces where some or all of them define a method with the
  // same signature.
  SignatureMap unimplemented;
};

using ClassSignatureMaps =
    InsertOnlyConcurrentMap<const DexClass*, ClassSignatureMap>;

using InterfaceSignatureMaps =
    InsertOnlyConcurrentMap<const DexClass*, SignatureMap>;

using UnifiedInterfacesSignatureMaps =
    InsertOnlyConcurrentMap<const DexTypeList*, SignatureMap>;

void update_signature_map(const DexMethod* method,
                          MethodSet value,
                          SignatureMap* map) {
  map->update(
      [&](const ProtoMap& protos) {
        auto copy = protos;
        copy.insert_or_assign(method->get_proto(), value);
        return copy;
      },
      method->get_name());
}

void unify_signature_maps(const SignatureMap& to_add, SignatureMap* target) {
  target->union_with(
      [&](const ProtoMap& p1, const ProtoMap& p2) {
        return p1.get_union_with(
            [&](const MethodSet& ms1, const MethodSet& ms2) {
              return ms1.get_union_with(ms2);
            },
            p2);
      },
      to_add);
}

class GraphBuilder {
 public:
  explicit GraphBuilder(const Scope& scope) : m_scope(scope) {}

  std::unique_ptr<Graph> run() {
    m_graph = std::make_unique<Graph>();
    walk::parallel::classes(m_scope, [&](const DexClass* cls) {
      if (is_interface(cls)) {
        analyze_interface(cls);
      } else {
        analyze_non_interface(cls);
      }
    });
    return std::move(m_graph);
  }

 private:
  const ClassSignatureMap& analyze_non_interface(const DexClass* cls) {
    always_assert(!is_interface(cls));
    auto* res = m_class_signature_maps.get(cls);
    if (res) {
      return *res;
    }

    // Initialize the signature maps from those of the superclass.
    ClassSignatureMap class_signatures;
    if (cls->get_super_class() != nullptr) {
      auto super_cls = type_class(cls->get_super_class());
      if (super_cls != nullptr) {
        class_signatures = analyze_non_interface(super_cls);
      }
    }

    // Add all methods from the interfaces that the current class directly
    // implements to the set of unimplemented methods.
    unify_signature_maps(unify_super_interface_signatures(cls),
                         &class_signatures.unimplemented);

    auto inherited_implemented = class_signatures.implemented;
    for (auto* method : cls->get_vmethods()) {
      // Replace the overridden methods by the overriding ones.
      update_signature_map(method, MethodSet{method},
                           &class_signatures.implemented);
    }

    // Find all implementation methods reachable via their interface methods.
    // Note that an interface method can be implemented by a method inherited
    // from a superclass.
    std::vector<std::pair<MethodSet, const DexMethod*>>
        unimplemented_implementations;
    for (const auto& protos_pair : class_signatures.unimplemented) {
      auto name = protos_pair.first;
      const auto& named_implemented_protos =
          class_signatures.implemented.at(name);
      if (named_implemented_protos.empty()) {
        continue;
      }
      for (const auto& ms_pair : protos_pair.second) {
        auto proto = ms_pair.first;
        const auto& implemented_set = named_implemented_protos.at(proto);
        if (implemented_set.empty()) {
          continue;
        }
        always_assert(implemented_set.size() == 1);
        auto implementation = *implemented_set.begin();
        unimplemented_implementations.emplace_back(ms_pair.second,
                                                   implementation);
      }
    }
    // Remove the newly implemented methods from the set of unimplemented
    // interface methods.
    for (auto&& [_, implementation] : unimplemented_implementations) {
      update_signature_map(implementation, MethodSet{},
                           &class_signatures.unimplemented);
    }

    auto [map_ptr, emplaced] =
        m_class_signature_maps.emplace(cls, class_signatures);
    if (emplaced) {
      // Mark all overriding methods as reachable via their parent method ref.
      for (auto* method : cls->get_vmethods()) {
        const auto& overridden_set =
            inherited_implemented.at(method->get_name())
                .at(method->get_proto());
        for (auto overridden : overridden_set) {
          m_graph->add_edge(overridden, /* overridden_is_interface */ false,
                            method, /* overriding_is_interface */ false);
        }
      }
      // Mark all implementation methods as reachable via their interface
      // methods.
      for (auto&& [unimplementeds, implementation] :
           unimplemented_implementations) {
        for (auto unimplemented : unimplementeds) {
          if (implementation->get_class() == cls->get_type() ||
              m_graph->add_other_implementation_class(unimplemented,
                                                      implementation, cls)) {
            m_graph->add_edge(unimplemented,
                              /* overridden_is_interface */ true,
                              implementation,
                              /* overriding_is_interface */ false);
          }
        }
      }
    }

    return *map_ptr;
  }

  const SignatureMap& analyze_interface(const DexClass* cls) {
    always_assert(is_interface(cls));
    auto* res = m_interface_signature_maps.get(cls);
    if (res) {
      return *res;
    }

    SignatureMap interface_signatures = unify_super_interface_signatures(cls);
    auto inherited_interface_signatures = interface_signatures;
    for (auto* method : cls->get_vmethods()) {
      update_signature_map(method, MethodSet{method}, &interface_signatures);
    }

    auto [map_ptr, emplaced] =
        m_interface_signature_maps.emplace(cls, interface_signatures);
    if (emplaced) {
      for (auto* method : cls->get_vmethods()) {
        const auto& overridden_set =
            inherited_interface_signatures.at(method->get_name())
                .at(method->get_proto());
        // These edges connect a method in a superinterface to the overriding
        // methods in a subinterface. A reference to the superinterface's method
        // will not resolve to the subinterface's method at runtime, but these
        // edges are critical because we do not add an edge between overridden
        // superinterface methods and their implementors. Concretely, given the
        // following code:
        //
        //   interface IA { void m(); }
        //   interface IB extends IA { void m(); }
        //   class C implements IB { void m(); }
        //
        // Our graph will contain an edge between IA::m and IB::m, and an edge
        // between IB::m and C::m. It will *not* contain an edge between IA::m
        // and C::m, even though C::m does implement IA::m as well. Therefore to
        // get all the implementors of IA::m, we need to traverse the edges
        // added here to find them. This design reduces the number of edges
        // necessary for building the graph.
        for (auto overridden : overridden_set) {
          m_graph->add_edge(overridden, /* overridden_is_interface */ true,
                            method, /* overriding_is_interface */ true);
        }
      }
    }

    return *map_ptr;
  }

  const SignatureMap& unify_super_interface_signatures(const DexClass* cls) {
    auto* type_list = cls->get_interfaces();
    auto* res = m_unified_interfaces_signature_maps.get(type_list);
    if (res) {
      return *res;
    }

    SignatureMap super_interface_signatures;
    for (auto* intf : *type_list) {
      auto intf_cls = type_class(intf);
      if (intf_cls != nullptr) {
        unify_signature_maps(analyze_interface(intf_cls),
                             &super_interface_signatures);
      }
    }

    auto [map_ptr, _] = m_unified_interfaces_signature_maps.emplace(
        type_list, super_interface_signatures);
    return *map_ptr;
  }

  std::unique_ptr<Graph> m_graph;
  ClassSignatureMaps m_class_signature_maps;
  InterfaceSignatureMaps m_interface_signature_maps;
  UnifiedInterfacesSignatureMaps m_unified_interfaces_signature_maps;
  const Scope& m_scope;
};

template <typename F>
bool all_overriding_methods_impl(const Graph& graph,
                                 const DexMethod* method,
                                 const F& f,
                                 bool include_interfaces,
                                 const DexType* base_type) {
  const Node& root = graph.get_node(method);
  if (base_type && method->get_class() == base_type) {
    base_type = nullptr;
  }
  if (root.is_interface) {
    UnorderedSet<const Node*> visited{&root};
    visited.reserve(root.children.size() * 7);
    return self_recursive_fn(
        [&](auto self, const auto& children) -> bool {
          for (const auto* node : UnorderedIterable(children)) {
            if (!visited.emplace(node).second) {
              continue;
            }
            if (!self(self, node->children)) {
              return false;
            }
            if ((include_interfaces || !node->is_interface) &&
                (!base_type || node->overrides(node->method, base_type)) &&
                !f(node->method)) {
              return false;
            }
          }
          return true;
        },
        root.children);
  }
  // optimized code path
  return self_recursive_fn(
      [&](auto self, const auto& children) -> bool {
        for (const auto* node : UnorderedIterable(children)) {
          if (!self(self, node->children)) {
            return false;
          }
          if ((!base_type || node->overrides(node->method, base_type)) &&
              !f(node->method)) {
            return false;
          }
        }
        return true;
      },
      root.children);
}

template <typename F>
bool all_overridden_methods_impl(const Graph& graph,
                                 const DexMethod* method,
                                 const F& f,
                                 bool include_interfaces) {
  const Node& root = graph.get_node(method);
  if (include_interfaces) {
    UnorderedSet<const Node*> visited{&root};
    visited.reserve(root.parents.size() * 7);
    return self_recursive_fn(
        [&](auto self, const auto& children) -> bool {
          for (const auto* node : UnorderedIterable(children)) {
            if (!visited.emplace(node).second) {
              continue;
            }
            if (!include_interfaces && node->is_interface) {
              continue;
            }
            if (!self(self, node->parents)) {
              return false;
            }
            if (!f(node->method)) {
              return false;
            }
          }
          return true;
        },
        root.parents);
  }
  if (root.is_interface) {
    return true;
  }
  // optimized code path
  return self_recursive_fn(
      [&](auto self, const auto& children) -> bool {
        for (const auto* node : UnorderedIterable(children)) {
          if (node->is_interface) {
            continue;
          }
          if (!self(self, node->parents)) {
            return false;
          }
          if (!f(node->method)) {
            return false;
          }
        }
        return true;
      },
      root.parents);
}

} // namespace

namespace method_override_graph {

Node Graph::empty_node;

bool Node::overrides(const DexMethod* current, const DexType* base_type) const {
  // Trivial case.
  if (type::check_cast(current->get_class(), base_type)) {
    return true;
  }
  // We also check if the current method was fulfilling an implementation
  // demand for any class that can be cast to the given base_type.
  if (!other_interface_implementations) {
    return false;
  }
  for (auto* cls :
       UnorderedIterable(other_interface_implementations->classes)) {
    if (type::check_cast(cls->get_type(), base_type)) {
      return true;
    }
  }
  return false;
}

const Node& Graph::get_node(const DexMethod* method) const {
  auto it = m_nodes.find(method);
  if (it == m_nodes.end()) {
    return empty_node;
  }
  return it->second;
}

void Graph::add_edge(const DexMethod* overridden, const DexMethod* overriding) {
  // The type-class lookup should only ever fail during testing if the
  // environment isn't fully build up.
  auto may_be_interface = [](DexType* t) {
    auto cls = type_class(t);
    return !cls || is_interface(cls);
  };
  add_edge(overridden, may_be_interface(overridden->get_class()), overriding,
           may_be_interface(overridden->get_class()));
}

void Graph::add_edge(const DexMethod* overridden,
                     bool overridden_is_interface,
                     const DexMethod* overriding,
                     bool overriding_is_interface) {
  Node* overriding_node = nullptr;
  m_nodes.update(overriding, [&](const DexMethod*, Node& node, bool exists) {
    if (!exists) {
      node.method = overriding;
      node.is_interface = overriding_is_interface;
    }
    overriding_node = &node;
  });

  Node* overridden_node = nullptr;
  m_nodes.update(overridden, [&](const DexMethod*, Node& node, bool exists) {
    if (exists) {
      always_assert(node.is_interface == overridden_is_interface);
    } else {
      node.method = overridden;
      node.is_interface = overridden_is_interface;
    }
    node.children.insert(overriding_node);
    overridden_node = &node;
  });

  m_nodes.update(overriding, [&](const DexMethod*, Node& node, bool exists) {
    redex_assert(exists);
    node.parents.insert(overridden_node);
  });
}

void Node::gather_connected_methods(
    UnorderedSet<const DexMethod*>* visited) const {
  if (method == nullptr) {
    return;
  }
  visited->insert(method);
  for (auto* child : UnorderedIterable(children)) {
    if (visited->count(child->method)) {
      continue;
    }
    child->gather_connected_methods(visited);
  }
  for (auto* parent : UnorderedIterable(parents)) {
    if (visited->count(parent->method)) {
      continue;
    }
    parent->gather_connected_methods(visited);
  }
}

bool Graph::add_other_implementation_class(const DexMethod* overridden,
                                           const DexMethod* overriding,
                                           const DexClass* cls) {
  bool parent_inserted = false;
  m_nodes.update(overriding, [&](const DexMethod*, Node& node, bool exists) {
    if (!exists) {
      node.method = overriding;
    }
    auto& oii = node.other_interface_implementations;
    if (!oii) {
      oii = std::make_unique<OtherInterfaceImplementations>();
    }
    oii->classes.insert(cls);
    parent_inserted = oii->parents.insert(overridden).second;
  });
  return parent_inserted;
}

void Graph::dump(std::ostream& os) const {
  namespace bs = binary_serialization;
  bs::write_header(os, /* version */ 1);
  bs::GraphWriter<const DexMethod*> gw(
      [&](std::ostream& os, const DexMethod* method) {
        const auto& s = show_deobfuscated(method);
        bs::write<uint32_t>(os, s.size());
        os << s;
      },
      [&](const DexMethod* method) -> std::vector<const DexMethod*> {
        const auto& node = get_node(method);
        std::vector<const DexMethod*> succs;
        succs.reserve(node.children.size());
        unordered_transform(node.children,
                            std::back_inserter(succs),
                            [](auto* c) { return c->method; });
        return succs;
      });
  gw.write(os, unordered_keys(m_nodes));
}

std::unique_ptr<const Graph> build_graph(const Scope& scope) {
  Timer t("Building method override graph");
  return GraphBuilder(scope).run();
}

UnorderedBag<const DexMethod*> get_overriding_methods(
    const Graph& graph,
    const DexMethod* method,
    bool include_interfaces,
    const DexType* base_type) {
  UnorderedBag<const DexMethod*> res;
  all_overriding_methods_impl(
      graph, method,
      [&](const DexMethod* method) {
        res.insert(method);
        return true;
      },
      include_interfaces, base_type);
  return res;
}

UnorderedBag<const DexMethod*> get_overridden_methods(const Graph& graph,
                                                      const DexMethod* method,
                                                      bool include_interfaces) {
  UnorderedBag<const DexMethod*> res;
  all_overridden_methods_impl(
      graph, method,
      [&](const DexMethod* method) {
        res.insert(method);
        return true;
      },
      include_interfaces);
  return res;
}

bool is_true_virtual(const Graph& graph, const DexMethod* method) {
  if (is_abstract(method)) {
    return true;
  }
  const auto& node = graph.get_node(method);
  return !node.parents.empty() || !node.children.empty();
}

InsertOnlyConcurrentSet<DexMethod*> get_non_true_virtuals(const Graph& graph,
                                                          const Scope& scope) {
  InsertOnlyConcurrentSet<DexMethod*> non_true_virtuals;
  workqueue_run<DexClass*>(
      [&](DexClass* cls) {
        for (auto* method : cls->get_vmethods()) {
          if (!is_true_virtual(graph, method)) {
            non_true_virtuals.insert(method);
          }
        }
      },
      scope);
  return non_true_virtuals;
}

bool all_overriding_methods(const Graph& graph,
                            const DexMethod* method,
                            const std::function<bool(const DexMethod*)>& f,
                            bool include_interfaces,
                            const DexType* base_type) {
  return all_overriding_methods_impl(graph, method, f, include_interfaces,
                                     base_type);
}

bool any_overriding_methods(const Graph& graph,
                            const DexMethod* method,
                            const std::function<bool(const DexMethod*)>& f,
                            bool include_interfaces,
                            const DexType* base_type) {
  return !all_overriding_methods_impl(
      graph, method, [&](const DexMethod* m) { return !f(m); },
      include_interfaces, base_type);
}

bool all_overridden_methods(const Graph& graph,
                            const DexMethod* method,
                            const std::function<bool(const DexMethod*)>& f,
                            bool include_interfaces) {
  return all_overridden_methods_impl(graph, method, f, include_interfaces);
}

bool any_overridden_methods(const Graph& graph,
                            const DexMethod* method,
                            const std::function<bool(const DexMethod*)>& f,
                            bool include_interfaces) {
  return !all_overridden_methods_impl(
      graph, method, [&](const DexMethod* m) { return !f(m); },
      include_interfaces);
}

UnorderedSet<DexClass*> get_classes_with_overridden_finalize(
    const Graph& method_override_graph, const ClassHierarchy& class_hierarchy) {
  UnorderedSet<DexClass*> res;
  auto overriding_methods = method_override_graph::get_overriding_methods(
      method_override_graph, method::java_lang_Object_finalize());
  for (auto* overriding_method : UnorderedIterable(overriding_methods)) {
    auto type = overriding_method->get_class();
    auto* cls = type_class(type);
    if (cls && !cls->is_external()) {
      res.insert(cls);
      auto children = get_all_children(class_hierarchy, type);
      for (auto child : children) {
        auto* child_cls = type_class(child);
        if (child_cls && !child_cls->is_external()) {
          res.insert(child_cls);
        }
      }
    }
  }
  return res;
}

} // namespace method_override_graph
