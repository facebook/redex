/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReachableClasses.h"
#include "RemoveUnreachable.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "Show.h"

#include <string>

/**
 * RemoveUnreachable eliminates unreachable code (classes, methods, and fields)
 * by using a mark-sweep algorithm.
 *
 * Conceptually we start at roots, which are defined by -keep rules in the
 * config file, and perform a depth-first search to find all references.
 * Elements visited in this manner will be retained, and are found in the
 * marked_* sets.
 *
 * -keepclassmembers rules are a bit more complicated, because they require
 * "conditional" marking: these members are kept only if their containing class
 * is determined to be kept.  The conditional marking logic is also used to
 * retain (or not) implementations of interface methods.  These elements are
 * placed in the cond_marked_* sets; care must be taken to promote
 * conditionally marked elements to fully marked
 */

namespace {

bool is_canary(const DexClass* cls) {
  return strstr(cls->get_name()->c_str(), "Canary");
}

DexMethod* resolve(const DexMethod* method, const DexClass* cls) {
  if (!cls) return nullptr;
  for (auto const& m : cls->get_vmethods()) {
    if (signatures_match(method, m)) {
      return m;
    }
  }
  for (auto const& m : cls->get_dmethods()) {
    if (signatures_match(method, m)) {
      return m;
    }
  }
  {
    auto const& superclass = type_class(cls->get_super_class());
    auto const resolved = resolve(method, superclass);
    if (resolved) {
      return resolved;
    }
  }
  for (auto const& interface : cls->get_interfaces()->get_type_list()) {
    auto const resolved = resolve(method, type_class(interface));
    if (resolved) {
      return resolved;
    }
  }
  return nullptr;
}

struct deleted_stats {
  size_t nclasses{0};
  size_t nfields{0};
  size_t nmethods{0};
};

deleted_stats trace_stats(const char* label, DexStoresVector& stores) {
  deleted_stats stats;
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    stats.nclasses += dex.size();
    for (auto const& cls : dex) {
      stats.nfields += cls->get_ifields().size();
      stats.nfields += cls->get_sfields().size();
      stats.nmethods += cls->get_dmethods().size();
      stats.nmethods += cls->get_vmethods().size();
    }
  }
  TRACE(RMU, 1, "%s: %lu classes, %lu fields, %lu methods\n",
        label, stats.nclasses, stats.nfields, stats.nmethods);
  return stats;
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
    m_inheritors[ancestor].insert(child);
    auto const& super_type = ancestor_cls->get_super_class();
    if (super_type) {
      TRACE(RMU, 4, "Child %s of %s\n", SHOW(child), SHOW(super_type));
      add_child(child, super_type);
    }
    auto const& interfaces = ancestor_cls->get_interfaces()->get_type_list();
    for (auto const& interface : interfaces) {
      TRACE(RMU, 4, "Child %s of %s\n", SHOW(child), SHOW(interface));
      add_child(child, interface);
    }
  }

 private:
  std::unordered_map<DexType*, std::unordered_set<DexType*>> m_inheritors;
};

bool implements_library_method(
  const DexMethod* to_check,
  const DexClass* cls
) {
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

bool implements_library_method(
  InheritanceGraph& graph,
  const DexMethod* to_check,
  const DexClass* cls
) {
  for (auto child : graph.get_descendants(cls->get_type())) {
    if (implements_library_method(to_check, type_class(child))) {
      return true;
    }
  }
  return false;
}

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

  void push(const DexType* type) {
    if (is_array(type)) {
      type = get_array_type(type);
    }
    push(type_class(type));
  }

  void push(const DexClass* cls) {
    if (!cls || marked(cls)) return;
    mark(cls);
    m_class_stack.emplace_back(cls);
  }

  void push(const DexField* field) {
    if (!field || marked(field)) return;
    mark(field);
    m_field_stack.emplace_back(field);
  }

  void push_cond(const DexField* field) {
    if (!field || marked(field)) return;
    TRACE(RMU, 4, "Conditionally marking field: %s\n", SHOW(field));
    if (marked(type_class(field->get_class()))) {
      push(field);
    } else {
      m_cond_marked_fields.emplace(field);
    }
  }

  void push(const DexMethod* method) {
    if (!method || marked(method)) return;
    mark(method);
    m_method_stack.emplace_back(method);
  }

  void push_cond(const DexMethod* method) {
    if (!method || marked(method)) return;
    TRACE(RMU, 4, "Conditionally marking method: %s\n", SHOW(method));
    if (marked(type_class(method->get_class()))) {
      push(method);
    } else {
      m_cond_marked_methods.emplace(method);
    }
  }

  template<typename T>
  void gather_and_push(T t) {
    std::vector<DexString*> strings;
    std::vector<DexType*> types;
    std::vector<DexField*> fields;
    std::vector<DexMethod*> methods;
    t->gather_strings(strings);
    t->gather_types(types);
    t->gather_fields(fields);
    t->gather_methods(methods);
    for (auto const& str : strings) {
      auto internal = JavaNameUtil::external_to_internal(str->c_str());
      auto typestr = DexString::get_string(internal.c_str());
      if (!typestr) continue;
      auto type = DexType::get_type(typestr);
      if (!type) continue;
      push(type);
    }
    for (auto const& type : types) {
      push(type);
    }
    for (auto const& field : fields) {
      push(field);
    }
    for (auto const& method : methods) {
      push(method);
    }
  }

  void visit(const DexClass* cls) {
    TRACE(RMU, 4, "Visiting class: %s\n", SHOW(cls));
    for (auto& m : cls->get_dmethods()) {
      if (is_clinit(m)) push(m);
    }
    push(type_class(cls->get_super_class()));
    for (auto const& t : cls->get_interfaces()->get_type_list()) {
      push(t);
    }
    const DexAnnotationSet* annoset = cls->get_anno_set();
    if (annoset) {
      for (auto const& anno : annoset->get_annotations()) {
        if (anno->type() == DexType::get_type("Ldalvik/annotation/MemberClasses;")) {
          // Ignore inner-class annotations.
          continue;
        }
        gather_and_push(anno);
      }
    }
    for (auto const& m : cls->get_ifields()) {
      if (m_cond_marked_fields.count(m)) {
        push(m);
      }
    }
    for (auto const& m : cls->get_sfields()) {
      if (m_cond_marked_fields.count(m)) {
        push(m);
      }
    }
    for (auto const& m : cls->get_dmethods()) {
      if (m_cond_marked_methods.count(m)) {
        push(m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (m_cond_marked_methods.count(m)) {
        push(m);
      }
    }
  }

  void visit(DexField* field) {
    TRACE(RMU, 4, "Visiting field: %s\n", SHOW(field));
    if (!field->is_concrete()) {
      auto const& realfield = resolve_field(
        field->get_class(), field->get_name(), field->get_type());
      push(realfield);
    }
    gather_and_push(field);
    push(field->get_class());
    push(field->get_type());
  }

  void visit(DexMethod* method) {
    TRACE(RMU, 4, "Visiting method: %s\n", SHOW(method));
    auto resolved_method = resolve(method, type_class(method->get_class()));
    TRACE(RMU, 5, "    Resolved to: %s\n", SHOW(resolved_method));
    push(resolved_method);
    gather_and_push(method);
    push(method->get_class());
    push(method->get_proto()->get_rtype());
    for (auto const& t : method->get_proto()->get_args()->get_type_list()) {
      push(t);
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
              push_cond(m);
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
        if (root(cls) || is_canary(cls)) {
          TRACE(RMU, 3, "Visiting seed: %s\n", SHOW(cls));
          push(cls);
        }
        for (auto const& f : cls->get_ifields()) {
          if (root(f) || is_volatile(f)) {
            TRACE(RMU, 3, "Visiting seed: %s\n", SHOW(f));
            push_cond(f);
          }
        }
        for (auto const& f : cls->get_sfields()) {
          if (root(f)) {
            TRACE(RMU, 3, "Visiting seed: %s\n", SHOW(f));
            push_cond(f);
          }
        }
        for (auto const& m : cls->get_dmethods()) {
          if (root(m)) {
            TRACE(RMU, 3, "Visiting seed: %s\n", SHOW(m));
            push_cond(m);
          }
        }
        for (auto const& m : cls->get_vmethods()) {
          if (root(m) || implements_library_method(m_inheritance_graph, m, cls)) {
            TRACE(RMU, 3, "Visiting seed: %s\n", SHOW(m));
            push_cond(m);
          }
        }
      }
    }
    while (true) {
      if (!m_class_stack.empty()) {
        auto cls = m_class_stack.back();
        m_class_stack.pop_back();
        visit(cls);
        continue;
      }
      if (!m_field_stack.empty()) {
        auto field = m_field_stack.back();
        m_field_stack.pop_back();
        visit(const_cast<DexField*>(field));
        continue;
      }
      if (!m_method_stack.empty()) {
        auto method = m_method_stack.back();
        m_method_stack.pop_back();
        visit(const_cast<DexMethod*>(method));
        continue;
      }
      return;
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
  std::unordered_set<const DexField*> m_cond_marked_fields;
  std::unordered_set<const DexMethod*> m_cond_marked_methods;
  std::vector<const DexClass*> m_class_stack;
  std::vector<const DexField*> m_field_stack;
  std::vector<const DexMethod*> m_method_stack;
};

}

void RemoveUnreachablePass::run_pass(
  DexStoresVector& stores,
  ConfigFiles& cfg,
  PassManager& pm
) {
  if (pm.no_proguard_rules()) {
    TRACE(RMU, 1, "RemoveUnreachablePass not run because no ProGuard configuration was provided.");
    return;
  }
  UnreachableCodeRemover ucr(stores);
  deleted_stats before = trace_stats("before", stores);
  ucr.mark_sweep();
  deleted_stats after = trace_stats("after", stores);
  pm.incr_metric("classes_removed", before.nclasses - after.nclasses);
  pm.incr_metric("fields_removed", before.nfields - after.nfields);
  pm.incr_metric("methods_removed", before.nmethods - after.nmethods);
}

static RemoveUnreachablePass s_pass;
