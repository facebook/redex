/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RemoveUnreachable.h"

#include "DexUtil.h"
#include "Resolver.h"

namespace {

template<class T>
bool is_seed(const T& t) {
  return t->rstate.is_seed() || t->rstate.is_renamed_seed();
}

bool signatures_match(const DexMethod* a, const DexMethod* b) {
  return a->get_name() == b->get_name() && a->get_proto() == b->get_proto();
}

bool implements_library_method(const DexMethod* to_check, const DexClass* cls) {
  if (!cls) return false;
  if (cls->is_external()) {
    for (auto const& m : cls->get_vmethods()) {
      if (signatures_match(to_check, m)) {
        return true;
      }
    }
  }
  auto const& superclass = type_class(cls->get_super_class());
  if (implements_library_method(to_check, superclass)) {
    return true;
  }
  for (auto const& interface : cls->get_interfaces()->get_type_list()) {
    if (implements_library_method(to_check, type_class(interface))) {
      return true;
    }
  }
  return false;
}

void trace_stats(const char* label, DexStoresVector& stores) {
  size_t nclasses = 0;
  size_t nfields = 0;
  size_t nmethods = 0;
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    nclasses += dex.size();
    for (auto const& cls : dex) {
      nfields += cls->get_ifields().size();
      nfields += cls->get_sfields().size();
      nmethods += cls->get_dmethods().size();
      nmethods += cls->get_vmethods().size();
    }
  }
  TRACE(RMU, 1, "%s: %lu classes, %lu fields, %lu methods\n",
        label, nclasses, nfields, nmethods);
}

struct InheritanceGraph {
  InheritanceGraph(DexStoresVector& stores) {
    for (auto const& dex : DexStoreClassesIterator(stores)) {
      for (auto const& cls : dex) {
        add_child(cls->get_type(), cls->get_type());
      }
    }
  }

  const std::unordered_set<DexType*>& get_descendants(DexType* type) {
    return m_inheritors[type];
  }

 private:
  void add_child(DexType* child, DexType* ancestor) {
    auto const& ancestor_cls = type_class(ancestor);
    if (!ancestor_cls) return;
    auto const& super_type = ancestor_cls->get_super_class();
    if (super_type) {
      TRACE(RMU, 3, "Child %s of %s\n", SHOW(child), SHOW(super_type));
      m_inheritors[super_type].insert(child);
      add_child(child, super_type);
    }
    auto const& interfaces = ancestor_cls->get_interfaces()->get_type_list();
    for (auto const& interface : interfaces) {
      TRACE(RMU, 3, "Child %s of %s\n", SHOW(child), SHOW(interface));
      m_inheritors[interface].insert(child);
      add_child(child, interface);
    }
  }

 private:
  std::unordered_map<DexType*, std::unordered_set<DexType*>> m_inheritors;
};

struct UnreachableCodeRemover {
  UnreachableCodeRemover(DexStoresVector& stores)
    : m_stores(stores),
      m_inheritance_graph(stores)
  {}

  void mark_sweep() {
    mark();
    sweep();
  }

 private:
  void mark(const DexClass* cls) {
    if (!cls) return;
    m_marked_classes.emplace(cls);
  }

  void mark(const DexField* field) {
    if (!field) return;
    m_marked_fields.emplace(field);
  }

  void mark(const DexMethod* method) {
    if (!method) return;
    m_marked_methods.emplace(method);
  }

  bool marked(const DexClass* cls) {
    return m_marked_classes.count(cls);
  }

  bool marked(const DexField* field) {
    return m_marked_fields.count(field);
  }

  bool marked(const DexMethod* method) {
    return m_marked_methods.count(method);
  }

  template<class T>
  void gather_and_visit(T t) {
    std::vector<DexType*> types;
    std::vector<DexField*> fields;
    std::vector<DexMethod*> methods;
    t->gather_types(types);
    t->gather_fields(fields);
    t->gather_methods(methods);
    for (auto const& t : types) {
      visit(type_class(t));
    }
    for (auto const& f : fields) {
      visit(f);
    }
    for (auto const& m : methods) {
      visit(m);
    }
  }

  void visit(const DexClass* cls) {
    if (!cls) return;
    if (marked(cls)) return;
    TRACE(RMU, 3, "Visiting class: %s\n", SHOW(cls));
    mark(cls);
    for (auto& m : cls->get_dmethods()) {
      if (is_init(m)) visit(m);
      if (is_clinit(m)) visit(m);
    }
    visit(type_class(cls->get_super_class()));
    for (auto const& t : cls->get_interfaces()->get_type_list()) {
      visit(type_class(t));
    }
    auto const& annoset = cls->get_anno_set();
    if (annoset) {
      gather_and_visit(annoset);
    }
  }

  void visit(DexField* field) {
    if (!field) return;
    if (marked(field)) return;
    TRACE(RMU, 3, "Visiting field: %s\n", SHOW(field));
    mark(field);
    if (!field->is_concrete()) {
      auto const& realfield = resolve_field(
        field->get_class(), field->get_name(), field->get_type());
      visit(realfield);
    }
    gather_and_visit(field);
    visit(type_class(field->get_class()));
    visit(type_class(field->get_type()));
  }

  void visit(DexMethod* method) {
    if (!method) return;
    if (marked(method)) return;
    TRACE(RMU, 3, "Visiting method: %s\n", SHOW(method));
    mark(method);
    visit(resolve_method(method, MethodSearch::Any));
    visit(resolve_intf_methodref(method));
    gather_and_visit(method);
    visit(type_class(method->get_class()));
    visit(type_class(method->get_proto()->get_rtype()));
    for (auto const& t : method->get_proto()->get_args()->get_type_list()) {
      visit(type_class(t));
    }
    if (method->is_virtual() || !method->is_concrete()) {
      // If we're keeping an interface method, we have to keep its
      // implementations.  Annoyingly, the implementation might be defined on a
      // super class of the class that implements the interface.
      auto const& cls = method->get_class();
      auto const& children = m_inheritance_graph.get_descendants(cls);
      for (auto child : children) {
        while (true) {
          auto child_cls = type_class(child);
          if (!child_cls || child_cls->is_external()) {
            break;
          }
          for (auto const& m : child_cls->get_vmethods()) {
            if (signatures_match(method, m)) {
              visit(m);
            }
          }
          child = child_cls->get_super_class();
        }
      }
    }
  }

  void mark() {
    for (auto const& dex : DexStoreClassesIterator(m_stores)) {
      for (auto const& cls : dex) {
        if (is_seed(cls)) {
          TRACE(RMU, 2, "Visiting seed: %s\n", SHOW(cls));
          visit(cls);
        }
        for (auto const& f : cls->get_ifields()) {
          if (is_seed(f)) {
            TRACE(RMU, 2, "Visiting seed: %s\n", SHOW(f));
            visit(f);
          }
        }
        for (auto const& f : cls->get_sfields()) {
          if (is_seed(f)) {
            TRACE(RMU, 2, "Visiting seed: %s\n", SHOW(f));
            visit(f);
          }
        }
        for (auto const& m : cls->get_dmethods()) {
          if (is_seed(m)) {
            TRACE(RMU, 2, "Visiting seed: %s\n", SHOW(m));
            visit(m);
          }
        }
        for (auto const& m : cls->get_vmethods()) {
          if (is_seed(m) || implements_library_method(m, cls)) {
            TRACE(RMU, 2, "Visiting seed: %s\n", SHOW(m));
            visit(m);
          }
        }
      }
    }
  }

  template<class Container, class Marked>
  void sweep_if_unmarked(
    Container& c,
    const std::unordered_set<Marked>& marked
  ) {
    c.erase(
      std::remove_if(
        c.begin(), c.end(),
        [&](const Marked& m) {
          if (marked.count(m) == 0) {
            TRACE(RMU, 2, "Removing %s\n", SHOW(m));
            return true;
          }
          return false;
        }),
      c.end());
  }

  void sweep() {
    for (auto& dex : DexStoreClassesIterator(m_stores)) {
      sweep_if_unmarked(dex, m_marked_classes);
      for (auto const& cls : dex) {
        sweep_if_unmarked(cls->get_ifields(), m_marked_fields);
        sweep_if_unmarked(cls->get_sfields(), m_marked_fields);
        sweep_if_unmarked(cls->get_dmethods(), m_marked_methods);
        sweep_if_unmarked(cls->get_vmethods(), m_marked_methods);
      }
    }
  }

 private:
  DexStoresVector& m_stores;
  InheritanceGraph m_inheritance_graph;
  std::unordered_set<const DexClass*> m_marked_classes;
  std::unordered_set<const DexField*> m_marked_fields;
  std::unordered_set<const DexMethod*> m_marked_methods;
};

}

void RemoveUnreachablePass::run_pass(
  DexStoresVector& stores,
  ConfigFiles& cfg,
  PassManager& pm
) {
  if (!cfg.using_seeds) {
    TRACE(RMU, 1, "No seeds information.  Not running RemoveUnreachable.\n");
    return;
  }
  UnreachableCodeRemover ucr(stores);
  trace_stats("before", stores);
  ucr.mark_sweep();
  trace_stats("after", stores);
}
