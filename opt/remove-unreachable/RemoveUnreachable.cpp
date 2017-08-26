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

/**
 * Define this to TRACE the reasons why each object is reachable.
 */
//#define DEBUG_UNREACHABLE

namespace {

#ifdef DEBUG_UNREACHABLE
enum ReachableObjectType {
  ANNO,
  CLASS,
  FIELD,
  METHOD,
  SEED,
};

/**
 * Represents an object (class, method, or field) that's considered reachable by this pass.
 *
 * Used for logging what retains what so that we can see what things which
 * should be removed aren't being removed.
 */
struct ReachableObject {
  const ReachableObjectType type;
  const DexAnnotation* anno;
  const DexClass* cls;
  const DexField* field;
  const DexMethod* method;

  explicit ReachableObject(const DexAnnotation* anno) : type{ReachableObjectType::ANNO},   anno{anno} {}
  explicit ReachableObject(const DexClass* cls) :       type{ReachableObjectType::CLASS},  cls{cls} {}
  explicit ReachableObject(const DexMethod* method) :   type{ReachableObjectType::METHOD}, method{method} {}
  explicit ReachableObject(const DexField* field) :     type{ReachableObjectType::FIELD},  field{field} {}
  explicit ReachableObject() : type{ReachableObjectType::SEED} {}

  std::string str() const {
    std::string result;
    switch (type) {
      case ReachableObjectType::ANNO:
        return show(anno);
      case ReachableObjectType::CLASS:
        return show(cls);
      case ReachableObjectType::FIELD:
        return show(field);
      case ReachableObjectType::METHOD:
        return show(method);
      case ReachableObjectType::SEED:
        return std::string("<explicitly kept due to is_seed()>");
    }
  }
};

struct ReachableObjectHash {
  std::size_t operator()(const ReachableObject obj) const {
    switch (obj.type) {
      case ReachableObjectType::ANNO:   return std::hash<const DexAnnotation*>{}(obj.anno);
      case ReachableObjectType::CLASS:  return std::hash<const DexClass*>{}(obj.cls);
      case ReachableObjectType::FIELD:  return std::hash<const DexField*>{}(obj.field);
      case ReachableObjectType::METHOD: return std::hash<const DexMethod*>{}(obj.method);
      case ReachableObjectType::SEED:   return 0;
    }
  }
};

struct ReachableObjectEq {
  bool operator()(const ReachableObject lhs, const ReachableObject rhs) const {
    if (lhs.type != rhs.type) {
      return false;
    }
    switch (lhs.type) {
      case ReachableObjectType::ANNO:   return lhs.anno == rhs.anno;
      case ReachableObjectType::CLASS:  return lhs.cls == rhs.cls;
      case ReachableObjectType::FIELD:  return lhs.field == rhs.field;
      case ReachableObjectType::METHOD: return lhs.method == rhs.method;
      case ReachableObjectType::SEED:   return true;
    }
  }
};

using ReachableObjectSet = std::unordered_set<ReachableObject, ReachableObjectHash, ReachableObjectEq>;
static std::unordered_map<ReachableObject, ReachableObjectSet, ReachableObjectHash, ReachableObjectEq> retainers_of;
static ReachableObject SEED_SINGLETON{};

void print_reachable_stack_h(const ReachableObject& obj) {
  TRACE(RMU, 1, "    %s\n", obj.str().c_str());
  if (obj.type == SEED) {
    return;
  }
  auto const& retainer_set = retainers_of[obj];
  if (retainer_set.empty()) {
    return; // Shouldn't happen, but...
  }
  print_reachable_stack_h(*retainer_set.cbegin());
}

template<class Reachable>
void print_reachable_stack(Reachable* r) {
  ReachableObject obj(r);
  TRACE(RMU, 1, "%s is reachable via\n", obj.str().c_str());
  auto const& retainer_set = retainers_of[obj];
  if (retainer_set.empty()) {
    return; // Shouldn't happen, but...
  }
  print_reachable_stack_h(*retainer_set.cbegin());
}

template<class Reachable>
void print_reachable_reason(Reachable* reachable) {
  ReachableObject obj(reachable);
  std::string reason = obj.str() + " is reachable via [";
  bool any_added = false;
  auto retainer_set = retainers_of[obj];
  for (auto& item : retainer_set) {
    if (any_added) {
      reason += ", ";
    }
    reason += item.str();
    any_added = true;
  }
  reason += "]";

  TRACE(RMU, 1, "%s\n", reason.c_str());
}


/*
 * We use templates to specialize record_reachability(parent, child) such that:
 *
 *  1. It works for all combinations of
 *       parent, child in {DexAnnotation*, DexClass*, DexType*, DexMethod*, DexField*}
 *
 *  2. We record the reachability relationship iff
 *       DEBUG_UNREACHABLE and neither type is DexType
 *
 *  3. If either argument is a DexType*, we extract the corresponding DexClass*
 *       and then call the right version of record_reachability()
 */
template<class Seed>
void record_is_seed(Seed* seed) {
  assert(seed != nullptr);
  ReachableObject seed_object(seed);
  retainers_of[seed_object].insert(SEED_SINGLETON);
}

template<class Parent, class Object>
struct RecordImpl {
  static void record_reachability(const Parent* parent, const Object* object) {
    assert(parent != nullptr && object != nullptr);
    ReachableObject reachable_obj(object);
    retainers_of[reachable_obj].emplace(parent);
  }
};

template<class Object>
struct RecordImpl<DexType, Object> {
  static void record_reachability(const DexType* parent, const Object* object) {
    DexClass* parent_cls = type_class(get_array_type_or_self(parent));
    // If parent_class is null then it's not ours (e.g. String), so skip it
    if (parent_cls) {
      RecordImpl<DexClass, Object>::record_reachability(parent_cls, object);
    }
  }
};

template<class Parent>
struct RecordImpl<Parent, DexType> {
  static void record_reachability(const Parent* parent, const DexType* object) {
    DexClass* object_cls = type_class(get_array_type_or_self(object));
    // If object_class is null then it's not ours (e.g. String), so skip it
    if (object_cls) {
      RecordImpl<Parent, DexClass>::record_reachability(parent, object_cls);
    }
  }
};

template<class Parent, class Object>
void record_reachability(Parent* parent, Object* object) {
  // We need to use this RecordImpl struct trick in order to
  // partially specialize the template
  RecordImpl<Parent, Object>::record_reachability(parent, object);
}

#else // DEBUG_UNREACHABLE is false

template<class Seed>
void record_is_seed(Seed* seed) { /* Do nothing */ }

template<class Seed>
inline void record_reachability(Seed* seed) { /* Do nothing */ }

template<class Parent, class Object>
inline void record_reachability(Parent* parent, Object* object) { /* Do nothing */ }

#endif

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
  UnreachableCodeRemover(
    PassManager& pm,
    DexStoresVector& stores,
      std::unordered_set<const DexType*>& ignore_string_literals_annos)
      : m_pm(pm),
        m_stores(stores),
        m_inheritance_graph(stores),
        m_ignore_string_literals_annos(ignore_string_literals_annos)
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

  void mark(const DexFieldRef* field) {
    if (!field) return;
    m_marked_fields.emplace(field);
  }

  void mark(const DexMethodRef* method) {
    if (!method) return;
    m_marked_methods.emplace(method);
  }

  bool marked(const DexClass* cls) {
    return m_marked_classes.count(cls);
  }

  bool marked(const DexFieldRef* field) {
    return m_marked_fields.count(field);
  }

  bool marked(const DexMethodRef* method) {
    return m_marked_methods.count(method);
  }

  void push_seed(const DexType* type) {
    type = get_array_type_or_self(type);
    push_seed(type_class(type));
  }

  template<class Parent>
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

  template<class Parent>
  void push(const Parent* parent, const DexClass* cls) {
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
    TRACE(RMU, 4, "Conditionally marking field: %s\n", SHOW(field));
    auto clazz = type_class(field->get_class());

    if (marked(clazz)) {
      push(clazz, field);
    } else {
      m_cond_marked_fields.emplace(field);
    }
  }

  template<class Parent>
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

  template<class Parent>
  void push(const Parent* parent, const DexMethodRef* method) {
    if (!method || marked(method)) return;
    record_reachability(parent, method);
    mark(method);
    m_method_stack.emplace_back(method);
  }

  void push_cond(const DexMethod* method) {
    if (!method || marked(method)) return;
    TRACE(RMU, 4, "Conditionally marking method: %s\n", SHOW(method));
    auto clazz = type_class(method->get_class());
    if (marked(clazz)) {
      push(clazz, method);
    } else {
      m_cond_marked_methods.emplace(method);
    }
  }

  void gather_and_push(DexMethod* meth) {
    auto* cls = type_class(meth->get_class());
    bool check_strings = true;
    if (cls) {
      for (const auto& anno_type : m_ignore_string_literals_annos) {
        if (has_anno(cls, anno_type)) {
          m_pm.incr_metric("num_ignore_check_strings", 1);
          check_strings = false;
          break;
        }
      }
    }
    gather_and_push(meth, check_strings);
  }

  template<typename T>
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
    TRACE(RMU, 4, "Visiting class: %s\n", SHOW(cls));
    for (auto& m : cls->get_dmethods()) {
      if (is_clinit(m)) push(cls, m);
      if (is_init(m)) {
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
        if (anno->type() == DexType::get_type("Ldalvik/annotation/MemberClasses;")) {
          // Ignore inner-class annotations.
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
    TRACE(RMU, 4, "Visiting field: %s\n", SHOW(field));
    if (!field->is_concrete()) {
      auto const& realfield = resolve_field(
        field->get_class(), field->get_name(), field->get_type());
      push(field, realfield);
    }
    push(field, field->get_class());
    push(field, field->get_type());
  }

  void visit(DexMethodRef* method) {
    TRACE(RMU, 4, "Visiting method: %s\n", SHOW(method));
    auto resolved_method = resolve(method, type_class(method->get_class()));
    if (resolved_method != nullptr) {
      TRACE(RMU, 5, "    Resolved to: %s\n", SHOW(resolved_method));
      push(method, resolved_method);
      gather_and_push(resolved_method);
    }
    push(method, method->get_class());
    push(method, method->get_proto()->get_rtype());
    for (auto const& t : method->get_proto()->get_args()->get_type_list()) {
      push(method, t);
    }
    if (method->is_def() && (
        static_cast<DexMethod*>(method)->is_virtual() ||
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

  void mark() {
    for (auto const& dex : DexStoreClassesIterator(m_stores)) {
      for (auto const& cls : dex) {
        if (root(cls) || is_canary(cls)) {
          TRACE(RMU, 3, "Visiting seed: %s\n", SHOW(cls));
          push_seed(cls);
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
        visit(const_cast<DexFieldRef*>(field));
        continue;
      }
      if (!m_method_stack.empty()) {
        auto method = m_method_stack.back();
        m_method_stack.pop_back();
        visit(const_cast<DexMethodRef*>(method));
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
  PassManager& m_pm;
  DexStoresVector& m_stores;
  InheritanceGraph m_inheritance_graph;
  std::unordered_set<const DexClass*> m_marked_classes;
  std::unordered_set<const DexFieldRef*> m_marked_fields;
  std::unordered_set<const DexMethodRef*> m_marked_methods;
  std::unordered_set<const DexField*> m_cond_marked_fields;
  std::unordered_set<const DexMethod*> m_cond_marked_methods;
  std::vector<const DexClass*> m_class_stack;
  std::vector<const DexFieldRef*> m_field_stack;
  std::vector<const DexMethodRef*> m_method_stack;
  std::unordered_set<const DexType*>& m_ignore_string_literals_annos;
};

}

void RemoveUnreachablePass::run_pass(
  DexStoresVector& stores,
  ConfigFiles& cfg,
  PassManager& pm
) {
  if (pm.no_proguard_rules()) {
    TRACE(RMU, 1,
        "RemoveUnreachablePass not run because no "
        "ProGuard configuration was provided.");
    return;
  }
  // load ignore string literals annotations
  std::unordered_set<const DexType*> ignore_string_literals_annos;
  for (const auto& name : m_ignore_string_literals) {
    const auto type = DexType::get_type(name.c_str());
    if (type != nullptr) {
      ignore_string_literals_annos.insert(type);
    }
  }
  UnreachableCodeRemover ucr(pm, stores, ignore_string_literals_annos);
  deleted_stats before = trace_stats("before", stores);
  ucr.mark_sweep();

  deleted_stats after = trace_stats("after", stores);
  pm.incr_metric("classes_removed", before.nclasses - after.nclasses);
  pm.incr_metric("fields_removed", before.nfields - after.nfields);
  pm.incr_metric("methods_removed", before.nmethods - after.nmethods);

#ifdef DEBUG_UNREACHABLE
  // Print out the reason that each class is being kept.
  for (auto& dex : DexStoreClassesIterator(stores)) {
   for (auto const& cls : dex) {
     print_reachable_stack(cls);
   }
  }
#endif
}

static RemoveUnreachablePass s_pass;
