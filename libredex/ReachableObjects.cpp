/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReachableObjects.h"

#include "DexUtil.h"
#include "Pass.h"
#include "ReachableClasses.h"
#include "Resolver.h"

using namespace reachable_objects;

/*
 * This helper class computes reachable objects by a DFS+marking algorithm.
 *
 * Conceptually we start at roots, which are defined by -keep rules in the
 * config file, and perform a depth-first search to find all references.
 * Elements visited in this manner will be retained, and are found in the
 * marked_* sets.
 *
 * -keepclassmembers rules are a bit more complicated, because they require
 * "conditional" marking: these members are kept only if their containing class
 * is determined to be kept. The conditional marking logic is also used to
 * retain (or not) implementations of interface methods. These elements are
 * placed in the cond_marked_* sets; care must be taken to promote
 * conditionally marked elements to fully marked.
 */

namespace {

static ReachableObject SEED_SINGLETON{};

bool is_canary(const DexClass* cls) {
  return strstr(cls->get_name()->c_str(), "Canary");
}

DexMethod* resolve(const DexMethodRef* method, const DexClass* cls) {
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

// TODO: Can it be replaced by ClassHierarchy helpers? I tried to replace it,
// but the results were different.
struct InheritanceGraph {
  explicit InheritanceGraph(DexStoresVector& stores) {
    for (auto const& dex : DexStoreClassesIterator(stores)) {
      for (auto const& cls : dex) {
        add_child(cls->get_type(), cls->get_type());
      }
    }
  }

  const std::set<const DexType*, dextypes_comparator>& get_descendants(
      const DexType* type) const {
    if (m_inheritors.count(type)) {
      return m_inheritors.at(type);
    } else {
      static std::set<const DexType*, dextypes_comparator> empty;
      return empty;
    }
  }

 private:
  void add_child(DexType* child, DexType* ancestor) {
    auto const& ancestor_cls = type_class(ancestor);
    if (!ancestor_cls) return;
    m_inheritors[ancestor].insert(child);
    auto const& super_type = ancestor_cls->get_super_class();
    if (super_type) {
      TRACE(REACH, 4, "Child %s of %s\n", SHOW(child), SHOW(super_type));
      add_child(child, super_type);
    }
    auto const& interfaces = ancestor_cls->get_interfaces()->get_type_list();
    for (auto const& interface : interfaces) {
      TRACE(REACH, 4, "Child %s of %s\n", SHOW(child), SHOW(interface));
      add_child(child, interface);
    }
  }

 private:
  std::unordered_map<const DexType*,
                     std::set<const DexType*, dextypes_comparator>>
      m_inheritors;
};

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

bool implements_library_method(InheritanceGraph& graph,
                               const DexMethod* to_check,
                               const DexClass* cls) {
  for (auto child : graph.get_descendants(cls->get_type())) {
    if (implements_library_method(to_check, type_class(child))) {
      return true;
    }
  }
  return false;
}

class Reachable {
  DexStoresVector& m_stores;
  const std::unordered_set<const DexType*>& m_ignore_string_literals;
  const std::unordered_set<const DexType*>& m_ignore_string_literal_annos;
  std::unordered_set<const DexType*> m_ignore_system_annos;
  bool m_record_reachability;
  InheritanceGraph m_inheritance_graph;
  int m_num_ignore_check_strings = 0;
  std::unordered_set<const DexClass*> m_marked_classes;
  std::unordered_set<const DexFieldRef*> m_marked_fields;
  std::unordered_set<const DexMethodRef*> m_marked_methods;
  std::unordered_set<const DexField*> m_cond_marked_fields;
  std::unordered_set<const DexMethod*> m_cond_marked_methods;
  std::vector<const DexClass*> m_class_stack;
  std::vector<const DexFieldRef*> m_field_stack;
  std::vector<const DexMethodRef*> m_method_stack;
  ReachableObjectGraph m_retainers_of;

 public:
  Reachable(
      DexStoresVector& stores,
      const std::unordered_set<const DexType*>& ignore_string_literals,
      const std::unordered_set<const DexType*>& ignore_string_literal_annos,
      const std::unordered_set<const DexType*>& ignore_system_annos,
      bool record_reachability)
      : m_stores(stores),
        m_ignore_string_literals(ignore_string_literals),
        m_ignore_string_literal_annos(ignore_string_literal_annos),
        m_ignore_system_annos(ignore_system_annos),
        m_record_reachability(record_reachability),
        m_inheritance_graph(stores) {
    // To keep the backward compatability of this code, ensure that the
    // "MemberClasses" annotation is always in m_ignore_system_annos.
    m_ignore_system_annos.emplace(
        DexType::get_type("Ldalvik/annotation/MemberClasses;"));
  }

 private:
  void mark(const DexClass* cls) {
    if (!cls) return;
    m_marked_classes.emplace(cls);
  }

  void mark(const DexFieldRef* field) {
    if (!field) return;
    m_marked_fields.emplace(field);
  }

  void mark(const DexMethodRef* method) {
    if (!method) return;
    m_marked_methods.emplace(method);
  }

  bool marked(const DexClass* cls) { return m_marked_classes.count(cls); }

  bool marked(const DexFieldRef* field) { return m_marked_fields.count(field); }

  bool marked(const DexMethodRef* method) {
    return m_marked_methods.count(method);
  }

  void push_seed(const DexType* type) {
    type = get_array_type_or_self(type);
    push_seed(type_class(type));
  }

  template <class Parent>
  void push(const Parent* parent, const DexType* type) {
    type = get_array_type_or_self(type);
    push(parent, type_class(type));
  }

  void push_seed(const DexClass* cls) {
    if (!cls || marked(cls)) return;
    record_is_seed(cls);
    mark(cls);
    m_class_stack.emplace_back(cls);
  }

  template <class Parent>
  void push(const Parent* parent, const DexClass* cls) {
    // FIXME: Bug! Even if cls is already marked, we need to record its
    // reachability from parent to cls.
    if (!cls || marked(cls)) return;
    record_reachability(parent, cls);
    mark(cls);
    m_class_stack.emplace_back(cls);
  }

  void push_seed(const DexField* field) {
    if (!field || marked(field)) return;
    record_is_seed(field);
    mark(field);
    m_field_stack.emplace_back(field);
  }

  void push_cond(const DexField* field) {
    if (!field || marked(field)) return;
    TRACE(REACH, 4, "Conditionally marking field: %s\n", SHOW(field));
    auto clazz = type_class(field->get_class());

    if (marked(clazz)) {
      push(clazz, field);
    } else {
      m_cond_marked_fields.emplace(field);
    }
  }

  template <class Parent>
  void push(const Parent* parent, const DexFieldRef* field) {
    if (!field || marked(field)) return;
    if (field->is_def()) {
      gather_and_push(static_cast<const DexField*>(field));
    }
    record_reachability(parent, field);
    mark(field);
    m_field_stack.emplace_back(field);
  }

  void push_seed(const DexMethod* method) {
    if (!method || marked(method)) return;
    record_is_seed(method);
    mark(method);
    m_method_stack.emplace_back(method);
  }

  template <class Parent>
  void push(const Parent* parent, const DexMethodRef* method) {
    if (!method || marked(method)) return;
    record_reachability(parent, method);
    mark(method);
    m_method_stack.emplace_back(method);
  }

  void push_cond(const DexMethod* method) {
    if (!method || marked(method)) return;
    TRACE(REACH, 4, "Conditionally marking method: %s\n", SHOW(method));
    auto clazz = type_class(method->get_class());
    if (marked(clazz)) {
      push(clazz, method);
    } else {
      m_cond_marked_methods.emplace(method);
    }
  }

  void gather_and_push(DexMethod* meth) {
    auto* type = meth->get_class();
    auto* cls = type_class(type);
    bool check_strings = true;
    if (m_ignore_string_literals.count(type)) {
      ++m_num_ignore_check_strings;
      check_strings = false;
    }
    if (cls && check_strings) {
      for (const auto& ignore_anno_type : m_ignore_string_literal_annos) {
        if (has_anno(cls, ignore_anno_type)) {
          ++m_num_ignore_check_strings;
          check_strings = false;
          break;
        }
      }
    }
    gather_and_push(meth, check_strings);
  }

  template <typename T>
  void gather_and_push(T t, bool check_strings = true) {
    std::vector<DexString*> strings;
    std::vector<DexType*> types;
    std::vector<DexFieldRef*> fields;
    std::vector<DexMethodRef*> methods;
    t->gather_strings(strings);
    t->gather_types(types);
    t->gather_fields(fields);
    t->gather_methods(methods);
    if (check_strings) {
      for (auto const& str : strings) {
        auto internal = JavaNameUtil::external_to_internal(str->c_str());
        auto typestr = DexString::get_string(internal.c_str());
        if (!typestr) continue;
        auto type = DexType::get_type(typestr);
        if (!type) continue;
        push(t, type);
      }
    }
    for (auto const& type : types) {
      push(t, type);
    }
    for (auto const& field : fields) {
      push(t, field);
    }
    for (auto const& method : methods) {
      push(t, method);
    }
  }

  void visit(const DexClass* cls) {
    TRACE(REACH, 4, "Visiting class: %s\n", SHOW(cls));
    for (auto& m : cls->get_dmethods()) {
      if (is_clinit(m)) {
        push(cls, m);
      } else if (is_init(m)) {
        // Push the parameterless constructor, in case it's constructed via
        // .class or Class.forName()
        if (m->get_proto()->get_args()->get_type_list().size() == 0) {
          push(cls, m);
        }
      }
    }
    push(cls, type_class(cls->get_super_class()));
    for (auto const& t : cls->get_interfaces()->get_type_list()) {
      push(cls, t);
    }
    const DexAnnotationSet* annoset = cls->get_anno_set();
    if (annoset) {
      for (auto const& anno : annoset->get_annotations()) {
        if (m_ignore_system_annos.count(anno->type())) {
          TRACE(REACH,
                5,
                "Stop marking from %s by system anno: %s\n",
                SHOW(cls),
                SHOW(anno->type()));
          continue;
        }
        record_reachability(cls, anno);
        gather_and_push(anno);
      }
    }
    for (auto const& m : cls->get_ifields()) {
      if (m_cond_marked_fields.count(m)) {
        push(cls, m);
      }
    }
    for (auto const& m : cls->get_sfields()) {
      if (m_cond_marked_fields.count(m)) {
        push(cls, m);
      }
    }
    for (auto const& m : cls->get_dmethods()) {
      if (m_cond_marked_methods.count(m)) {
        push(cls, m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (m_cond_marked_methods.count(m)) {
        push(cls, m);
      }
    }
  }

  void visit(DexFieldRef* field) {
    TRACE(REACH, 4, "Visiting field: %s\n", SHOW(field));
    if (!field->is_concrete()) {
      auto const& realfield = resolve_field(
          field->get_class(), field->get_name(), field->get_type());
      push(field, realfield);
    }
    push(field, field->get_class());
    push(field, field->get_type());
  }

  void visit(DexMethodRef* method) {
    TRACE(REACH, 4, "Visiting method: %s\n", SHOW(method));
    auto resolved_method = resolve(method, type_class(method->get_class()));
    if (resolved_method != nullptr) {
      TRACE(REACH, 5, "    Resolved to: %s\n", SHOW(resolved_method));
      push(method, resolved_method);
      gather_and_push(resolved_method);
    }
    push(method, method->get_class());
    push(method, method->get_proto()->get_rtype());
    for (auto const& t : method->get_proto()->get_args()->get_type_list()) {
      push(method, t);
    }
    if (method->is_def() && (static_cast<DexMethod*>(method)->is_virtual() ||
                             !method->is_concrete())) {
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

  /*
   * We use templates to specialize record_reachability(parent, child) such
   * that:
   *
   *  1. It works for all combinations of parent, child in
   *     {DexAnnotation*, DexClass*, DexType*, DexMethod*, DexField*}
   *
   *  2. We record the reachability relationship iff
   *     m_record_reachability == true and neither type is DexType.
   *
   *  3. If either argument is a DexType*, we extract the corresponding
   *     DexClass* and then call the right version of record_reachability().
   */
  template <class Seed>
  void record_is_seed(Seed* seed) {
    if (m_record_reachability) {
      assert(seed != nullptr);
      m_retainers_of[ReachableObject(seed)].emplace(SEED_SINGLETON);
    }
  }

  template <class Parent, class Object>
  struct RecordImpl {
    static void record_reachability(const Parent* parent,
                                    const Object* object,
                                    ReachableObjectGraph& retainers_of) {
      assert(parent != nullptr && object != nullptr);
      retainers_of[ReachableObject(object)].emplace(parent);
    }
  };

  template <class Parent, class Object>
  void record_reachability(Parent* parent, Object* object) {
    if (m_record_reachability) {
      RecordImpl<Parent, Object>::record_reachability(
          parent, object, m_retainers_of);
    }
  }

 public:
  ReachableObjects mark(int* num_ignore_check_strings) {
    for (auto const& dex : DexStoreClassesIterator(m_stores)) {
      for (auto const& cls : dex) {
        if (root(cls) || is_canary(cls)) {
          TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(cls));
          push_seed(cls);
        }
        for (auto const& f : cls->get_ifields()) {
          if (root(f) || is_volatile(f)) {
            TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(f));
            push_cond(f);
          }
        }
        for (auto const& f : cls->get_sfields()) {
          if (root(f)) {
            TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(f));
            push_cond(f);
          }
        }
        for (auto const& m : cls->get_dmethods()) {
          if (root(m)) {
            TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(m));
            push_cond(m);
          }
        }
        for (auto const& m : cls->get_vmethods()) {
          if (root(m) ||
              implements_library_method(m_inheritance_graph, m, cls)) {
            TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(m));
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
        visit(const_cast<DexFieldRef*>(field));
        continue;
      }
      if (!m_method_stack.empty()) {
        auto method = m_method_stack.back();
        m_method_stack.pop_back();
        visit(const_cast<DexMethodRef*>(method));
        continue;
      }
      break;
    }

    if (num_ignore_check_strings) {
      *num_ignore_check_strings = m_num_ignore_check_strings;
    }

    ReachableObjects ret;
    ret.marked_fields = std::move(m_marked_fields);
    ret.marked_classes = std::move(m_marked_classes);
    ret.marked_methods = std::move(m_marked_methods);
    ret.retainers_of = std::move(m_retainers_of);
    return ret;
  }
};

void print_reachable_stack_h(const ReachableObject& obj,
                             ReachableObjectGraph& retainers_of,
                             const std::string& dump_tag) {
  TRACE(REACH_DUMP, 5, "%s    %s\n", dump_tag.c_str(), obj.str().c_str());
  if (obj.type == ReachableObjectType::SEED) {
    return;
  }
  auto const& retainer_set = retainers_of[obj];
  if (retainer_set.empty()) {
    return; // Shouldn't happen, but...
  }
  print_reachable_stack_h(*retainer_set.cbegin(), retainers_of, dump_tag);
}

template <class Reachable>
void print_reachable_stack(Reachable* r,
                           ReachableObjectGraph& retainers_of,
                           const std::string& dump_tag) {
  ReachableObject obj(r);
  TRACE(REACH_DUMP,
        5,
        "%s %s is reachable via\n",
        dump_tag.c_str(),
        obj.str().c_str());
  auto const& retainer_set = retainers_of[obj];
  if (retainer_set.empty()) {
    return; // Shouldn't happen, but...
  }
  print_reachable_stack_h(*retainer_set.cbegin(), retainers_of, dump_tag);
}

template <class Reachable>
void print_reachable_reason(Reachable* reachable,
                            ReachableObjectGraph& retainers_of,
                            const std::string& dump_tag) {
  ReachableObject obj(reachable);
  bool any_added = false;
  auto retainer_set = retainers_of[obj];
  std::string reason = obj.str() + " is reachable via " +
                       std::to_string(retainer_set.size()) + " [";
  for (auto& item : retainer_set) {
    if (any_added) {
      reason += ", ";
    }
    reason += item.str();
    any_added = true;
  }
  reason += "]";

  TRACE(REACH_DUMP, 5, "%s %s\n", dump_tag.c_str(), reason.c_str());
}

void print_graph_edges(const DexClass* cls,
                       ReachableObjectGraph& retainers_of,
                       const std::string& dump_tag,
                       std::ostream& os) {
  ReachableObject obj(cls);
  std::string s;
  s = "\"[" + obj.type_str() + "] " + obj.str() + "\"";
  while (true) {
    const auto& set = retainers_of[obj];
    if (set.empty()) {
      break;
    }
    s = "\t" + s;
    ReachableObject prev = obj;
    // NOTE: We only read the first item, but it seems fine. I didn't observe
    // any case of more than one item in set.
    assert(set.size() == 1);
    obj = *begin(set);
    if (obj.type == ReachableObjectType::SEED) {
      s = "\"[SEED] " + prev.str() + " " + prev.state_str() + "\"" + s;
      break;
    } else {
      s = "\"[" + obj.type_str() + "] " + obj.str() + "\"" + s;
    }
  }

  os << cls->get_deobfuscated_name() << "\t" << s << std::endl;
  TRACE(REACH_DUMP,
        5,
        "EDGE: %s %s %s;\n",
        dump_tag.c_str(),
        cls->get_deobfuscated_name().c_str(),
        s.c_str());
}
} // namespace

ReachableObjects compute_reachable_objects(
    DexStoresVector& stores,
    const std::unordered_set<const DexType*>& ignore_string_literals,
    const std::unordered_set<const DexType*>& ignore_string_literal_annos,
    const std::unordered_set<const DexType*>& ignore_system_annos,
    int* num_ignore_check_strings,
    bool record_reachability) {
  return Reachable(stores,
                   ignore_string_literals,
                   ignore_string_literal_annos,
                   ignore_system_annos,
                   record_reachability)
      .mark(num_ignore_check_strings);
}

void dump_reachability(DexStoresVector& stores,
                       ReachableObjectGraph& retainers_of,
                       const std::string& dump_tag) {
  for (const auto& dex : DexStoreClassesIterator(stores)) {
    for (const auto& cls : dex) {
      print_reachable_reason(cls, retainers_of, dump_tag);
      print_reachable_stack(cls, retainers_of, dump_tag);
    }
  }
}

void dump_reachability_graph(DexStoresVector& stores,
                             ReachableObjectGraph& retainers_of,
                             const std::string& dump_tag,
                             std::ostream& os) {
  for (const auto& dex : DexStoreClassesIterator(stores)) {
    for (const auto& cls : dex) {
      print_graph_edges(cls, retainers_of, dump_tag, os);
    }
  }
}
