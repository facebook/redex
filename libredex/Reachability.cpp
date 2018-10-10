/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Reachability.h"

#include "DexUtil.h"
#include "Pass.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Timer.h"
#include "Walkers.h"

using namespace reachability;

namespace mog = method_override_graph;

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

} // namespace

namespace reachability {

std::string ReachableObject::str() const {
  switch (type) {
  case ReachableObjectType::ANNO:
    return "[ANNO] " + show_deobfuscated(anno->type());
  case ReachableObjectType::CLASS:
    return show_deobfuscated(cls);
  case ReachableObjectType::FIELD:
    return show_deobfuscated(field);
  case ReachableObjectType::METHOD:
    return show_deobfuscated(method);
  case ReachableObjectType::SEED:
    return "<SEED>";
  }
}

/*
 * Initializes the root set by marking and pushing nodes onto the work queue.
 * Also conditionally marks class member seeds.
 */
void RootSetMarker::mark(const Scope& scope) {
  walk::parallel::classes(scope, [&](const DexClass* cls) {
    if (root(cls) || is_canary(cls)) {
      TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(cls));
      push_seed(cls);
    }
    for (auto const& f : cls->get_ifields()) {
      if (root(f) || is_volatile(f)) {
        TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(f));
        push_seed(f);
      }
    }
    for (auto const& f : cls->get_sfields()) {
      if (root(f)) {
        TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(f));
        push_seed(f);
      }
    }
    for (auto const& m : cls->get_dmethods()) {
      if (root(m)) {
        TRACE(REACH, 3, "Visiting seed: %s\n", SHOW(m));
        push_seed(m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (root(m)) {
        TRACE(REACH, 3, "Visiting seed: %s (root)\n", SHOW(m));
        push_seed(m);
      }
    }
  });

  mark_external_method_overriders();
}

void RootSetMarker::push_seed(const DexClass* cls) {
  if (!cls) return;
  record_is_seed(cls);
  m_reachable_objects->mark(cls);
  m_root_set->emplace(cls);
}

void RootSetMarker::push_seed(const DexField* field) {
  if (!field) return;
  record_is_seed(field);
  m_cond_marked->fields.insert(field);
}

void RootSetMarker::push_seed(const DexMethod* method) {
  if (!method) return;
  record_is_seed(method);
  m_cond_marked->methods.insert(method);
}

template <class Seed>
void RootSetMarker::record_is_seed(Seed* seed) {
  if (m_record_reachability) {
    assert(seed != nullptr);
    m_reachable_objects->record_is_seed(seed);
  }
}

/*
 * Mark as seeds all methods that override or implement an external method.
 */
void RootSetMarker::mark_external_method_overriders() {
  std::unordered_set<const DexMethod*> visited;
  for (auto& pair : m_method_override_graph.nodes()) {
    auto method = pair.first;
    if (!method->is_external() || visited.count(method)) {
      continue;
    }
    const auto& overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, method);
    for (auto* overriding : overriding_methods) {
      // Avoid re-visiting methods found in overriding sets since we would
      // already have conditionally marked all their children.
      visited.emplace(overriding);
      if (!overriding->is_external()) {
        TRACE(REACH, 3, "Visiting seed: %s (implements %s)\n", SHOW(overriding),
              SHOW(method));
        push_seed(overriding);
      }
    }
  }
}

/*
 * Marks :obj and pushes its immediately reachable neighbors onto the local
 * task queue of the current worker.
 */
void TransitiveClosureMarker::visit(const ReachableObject& obj) {
  switch (obj.type) {
  case ReachableObjectType::CLASS:
    visit(obj.cls);
    break;
  case ReachableObjectType::FIELD:
    visit(obj.field);
    break;
  case ReachableObjectType::METHOD:
    visit(obj.method);
    break;
  case ReachableObjectType::ANNO:
  case ReachableObjectType::SEED:
    always_assert_log(false, "Unexpected ReachableObject type");
    break;
  }
}

template <class Parent, class InputIt>
void TransitiveClosureMarker::push(const Parent* parent,
                                   InputIt begin,
                                   InputIt end) {
  for (auto it = begin; it != end; ++it) {
    push(parent, *it);
  }
}

template <class Parent>
void TransitiveClosureMarker::push(const Parent* parent, const DexType* type) {
  type = get_array_type_or_self(type);
  push(parent, type_class(type));
}

template <class Parent>
void TransitiveClosureMarker::push(const Parent* parent, const DexClass* cls) {
  // FIXME: Bug! Even if cls is already marked, we need to record its
  // reachability from parent to cls.
  if (!cls || m_reachable_objects->marked(cls)) return;
  record_reachability(parent, cls);
  m_reachable_objects->mark(cls);
  m_worker_state->push_task(ReachableObject(cls));
}

template <class Parent>
void TransitiveClosureMarker::push(const Parent* parent,
                                   const DexFieldRef* field) {
  if (!field || m_reachable_objects->marked(field)) return;
  if (field->is_def()) {
    gather_and_push(static_cast<const DexField*>(field));
  }
  record_reachability(parent, field);
  m_reachable_objects->mark(field);
  m_worker_state->push_task(ReachableObject(field));
}

template <class Parent>
void TransitiveClosureMarker::push(const Parent* parent,
                                   const DexMethodRef* method) {
  if (!method || m_reachable_objects->marked(method)) return;
  record_reachability(parent, method);
  m_reachable_objects->mark(method);
  m_worker_state->push_task(ReachableObject(method));
}

void TransitiveClosureMarker::push_cond(const DexMethod* method) {
  if (!method || m_reachable_objects->marked(method)) return;
  TRACE(REACH, 4, "Conditionally marking method: %s\n", SHOW(method));
  auto clazz = type_class(method->get_class());
  m_cond_marked->methods.insert(method);
  // If :clazz has been marked, we cannot count on visit(DexClass*) to move
  // the conditionally-marked methods into the actually-marked ones -- we have
  // to do it ourselves. Note that we must do this check after adding :method
  // to m_cond_marked to avoid a race condition where we add to m_cond_marked
  // after visit(DexClass*) has finished moving its contents over to
  // m_reachable_objects.
  if (m_reachable_objects->marked(clazz)) {
    push(clazz, method);
  }
}

template <class T>
static References generic_gather(T t) {
  References refs;
  t->gather_strings(refs.strings);
  t->gather_types(refs.types);
  t->gather_fields(refs.fields);
  t->gather_methods(refs.methods);
  return refs;
}

References TransitiveClosureMarker::gather(const DexAnnotation* anno) const {
  return generic_gather(anno);
}

References TransitiveClosureMarker::gather(const DexMethod* method) const {
  return generic_gather(method);
}

References TransitiveClosureMarker::gather(const DexField* field) const {
  return generic_gather(field);
}

void TransitiveClosureMarker::gather_and_push(DexMethod* meth) {
  auto* type = meth->get_class();
  auto* cls = type_class(type);
  bool check_strings = true;
  if (m_ignore_sets.string_literals.count(type)) {
    ++m_worker_state->get_data()->num_ignore_check_strings;
    check_strings = false;
  }
  if (cls && check_strings) {
    for (const auto& ignore_anno_type : m_ignore_sets.string_literal_annos) {
      if (has_anno(cls, ignore_anno_type)) {
        ++m_worker_state->get_data()->num_ignore_check_strings;
        check_strings = false;
        break;
      }
    }
  }
  auto refs = gather(meth);
  if (check_strings) {
    push_typelike_strings(meth, refs.strings);
  }
  push(meth, refs.types.begin(), refs.types.end());
  push(meth, refs.fields.begin(), refs.fields.end());
  push(meth, refs.methods.begin(), refs.methods.end());
}

template <typename T>
void TransitiveClosureMarker::gather_and_push(T t) {
  auto refs = gather(t);
  push_typelike_strings(t, refs.strings);
  push(t, refs.types.begin(), refs.types.end());
  push(t, refs.fields.begin(), refs.fields.end());
  push(t, refs.methods.begin(), refs.methods.end());
}

template <class Parent>
void TransitiveClosureMarker::push_typelike_strings(
    const Parent* parent, const std::vector<DexString*>& strings) {
  for (auto const& str : strings) {
    auto internal = JavaNameUtil::external_to_internal(str->c_str());
    auto type = DexType::get_type(internal.c_str());
    if (!type) {
      continue;
    }
    push(parent, type);
  }
}

void TransitiveClosureMarker::visit(const DexClass* cls) {
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
      if (m_ignore_sets.system_annos.count(anno->type())) {
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
    if (m_cond_marked->fields.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_sfields()) {
    if (m_cond_marked->fields.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_dmethods()) {
    if (m_cond_marked->methods.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_vmethods()) {
    if (m_cond_marked->methods.count(m)) {
      push(cls, m);
    }
  }
}

void TransitiveClosureMarker::visit(const DexFieldRef* field) {
  TRACE(REACH, 4, "Visiting field: %s\n", SHOW(field));
  if (!field->is_concrete()) {
    auto const& realfield =
        resolve_field(field->get_class(), field->get_name(), field->get_type());
    push(field, realfield);
  }
  push(field, field->get_class());
  push(field, field->get_type());
}

void TransitiveClosureMarker::visit(const DexMethodRef* method) {
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
  if (!method->is_def()) {
    return;
  }
  auto m = static_cast<const DexMethod*>(method);
  // If we're keeping an interface or virtual method, we have to keep its
  // implementations and overriding methods respectively.
  if (m->is_virtual() || !m->is_concrete()) {
    const auto& overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, m);
    for (auto* overriding : overriding_methods) {
      push_cond(overriding);
    }
  }
}

template <class Parent, class Object>
void TransitiveClosureMarker::record_reachability(Parent* parent,
                                                  Object* object) {
  if (m_record_reachability) {
    assert(parent != nullptr && object != nullptr);
    m_reachable_objects->record_reachability(parent, object);
  }
}

IgnoreSets::IgnoreSets(const JsonWrapper& jw) {
  auto parse_type_list = [&jw](const char* key,
                               std::unordered_set<const DexType*>* type_list) {
    std::vector<std::string> strs;
    jw.get(key, {}, strs);
    for (auto s : strs) {
      auto type = DexType::get_type(s);
      if (type != nullptr) {
        type_list->insert(type);
      }
    }
  };
  parse_type_list("ignore_string_literals", &string_literals);
  parse_type_list("ignore_string_literal_annos", &string_literal_annos);
  parse_type_list("ignore_system_annos", &system_annos);

  // To keep the backward compatability of this code, ensure that the
  // "MemberClasses" annotation is always in system_annos.
  system_annos.emplace(DexType::get_type("Ldalvik/annotation/MemberClasses;"));
}

std::unique_ptr<ReachableObjects> compute_reachable_objects(
    DexStoresVector& stores,
    const IgnoreSets& ignore_sets,
    int* num_ignore_check_strings,
    bool record_reachability) {
  Timer t("Marking");
  auto scope = build_class_scope(stores);
  auto reachable_objects = std::make_unique<ReachableObjects>();
  ConditionallyMarked cond_marked;
  auto method_override_graph = mog::build_graph(scope);

  ConcurrentSet<ReachableObject, ReachableObjectHash> root_set;
  RootSetMarker root_set_marker(*method_override_graph,
                                record_reachability,
                                &cond_marked,
                                reachable_objects.get(),
                                &root_set);
  root_set_marker.mark(scope);

  size_t num_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
  auto stats_arr = std::make_unique<Stats[]>(num_threads);
  MarkWorkQueue work_queue(
      [&](MarkWorkerState* worker_state, const ReachableObject& obj) {
        TransitiveClosureMarker transitive_closure_marker(
            ignore_sets, *method_override_graph, record_reachability,
            &cond_marked, reachable_objects.get(), worker_state);
        transitive_closure_marker.visit(obj);
        return nullptr;
      },
      [](std::nullptr_t, std::nullptr_t) { return nullptr; },
      [&stats_arr](unsigned int thread_idx) { return &stats_arr[thread_idx]; },
      num_threads);
  for (const auto& obj : root_set) {
    work_queue.add_item(obj);
  }
  work_queue.run_all();

  if (num_ignore_check_strings != nullptr) {
    for (size_t i = 0; i < num_threads; ++i) {
      *num_ignore_check_strings += stats_arr[i].num_ignore_check_strings;
    }
  }
  return reachable_objects;
}

/*
 * We use templates to specialize record_reachability(parent, child) such
 * that:
 *
 *  1. it works for all combinations of parent, child in
 *     {DexAnnotation*, DexClass*, DexType*, DexMethod*, DexField*}
 *
 *  2. We record the reachability relationship iff
 *     m_record_reachability == true.
 */
template <class Parent, class Object>
void ReachableObjects::record_reachability(Parent* parent, Object* object) {
  m_retainers_of.update(
      ReachableObject(object),
      [&](const ReachableObject&, ReachableObjectSet& set, bool /* exists */) {
        set.emplace(parent);
      });
}

template <class Seed>
void ReachableObjects::record_is_seed(Seed* seed) {
  assert(seed != nullptr);
  m_retainers_of.update(
      ReachableObject(seed),
      [&](const ReachableObject&, ReachableObjectSet& set, bool /* exists */) {
        set.emplace(SEED_SINGLETON);
      });
}

/*
 * Remove unmarked fields from :fields and erase their definitions from
 * g_redex.
 */
static void sweep_fields_if_unmarked(
    std::vector<DexField*>& fields,
    const ReachableObjects& reachables) {
  auto p = [&](DexField* f) {
    if (reachables.marked_unsafe(f) == 0) {
      TRACE(RMU, 2, "Removing %s\n", SHOW(f));
      DexField::erase_field(f);
      return true;
    }
    return false;
  };
  fields.erase(std::remove_if(fields.begin(), fields.end(), p), fields.end());
}

/*
 * Remove unmarked classes and methods. This should really erase the classes /
 * methods from g_redex as well, but that will probably result in dangling
 * pointers (at least for DexMethods). We should fix that at some point...
 */
template <class Container>
static void sweep_if_unmarked(Container& c,
                              const ReachableObjects& reachables) {
  auto p = [&](const auto& m) { return !reachables.marked_unsafe(m); };
  c.erase(std::remove_if(c.begin(), c.end(), p), c.end());
}

void sweep(DexStoresVector& stores, const ReachableObjects& reachables) {
  Timer t("Sweep");
  for (auto& dex : DexStoreClassesIterator(stores)) {
    sweep_if_unmarked(dex, reachables);
    walk::parallel::classes(dex, [&](DexClass* cls) {
      sweep_fields_if_unmarked(cls->get_ifields(), reachables);
      sweep_fields_if_unmarked(cls->get_sfields(), reachables);
      sweep_if_unmarked(cls->get_dmethods(), reachables);
      sweep_if_unmarked(cls->get_vmethods(), reachables);
    });
  }
}

ObjectCounts count_objects(const DexStoresVector& stores) {
  ObjectCounts counts;
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    counts.num_classes += dex.size();
    for (auto const& cls : dex) {
      counts.num_fields += cls->get_ifields().size();
      counts.num_fields += cls->get_sfields().size();
      counts.num_methods += cls->get_dmethods().size();
      counts.num_methods += cls->get_vmethods().size();
    }
  }
  return counts;
}

namespace {

void print_reachable_stack_h(const ReachableObject& obj,
                             const ReachableObjectGraph& retainers_of,
                             const std::string& dump_tag) {
  TRACE(REACH_DUMP, 5, "%s    %s\n", dump_tag.c_str(), obj.str().c_str());
  if (obj.type == ReachableObjectType::SEED) {
    return;
  }
  auto const& retainer_set = retainers_of.at_unsafe(obj);
  if (retainer_set.empty()) {
    return; // Shouldn't happen, but...
  }
  print_reachable_stack_h(*retainer_set.cbegin(), retainers_of, dump_tag);
}

template <class Reachable>
void print_reachable_stack(Reachable* r,
                           const ReachableObjectGraph& retainers_of,
                           const std::string& dump_tag) {
  ReachableObject obj(r);
  TRACE(REACH_DUMP,
        5,
        "%s %s is reachable via\n",
        dump_tag.c_str(),
        obj.str().c_str());
  auto const& retainer_set = retainers_of.at_unsafe(obj);
  if (retainer_set.empty()) {
    return; // Shouldn't happen, but...
  }
  print_reachable_stack_h(*retainer_set.cbegin(), retainers_of, dump_tag);
}

template <class Reachable>
void print_reachable_reason(Reachable* reachable,
                            const ReachableObjectGraph& retainers_of,
                            const std::string& dump_tag) {
  ReachableObject obj(reachable);
  bool any_added = false;
  const auto& retainer_set = retainers_of.at_unsafe(obj);
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
                       const ReachableObjectGraph& retainers_of,
                       const std::string& dump_tag,
                       std::ostream& os) {
  ReachableObject obj(cls);
  std::string s;
  s = "\"[" + obj.type_str() + "] " + obj.str() + "\"";
  while (true) {
    const auto& set = retainers_of.at_unsafe(obj);
    if (set.empty()) {
      break;
    }
    s = "\t" + s;
    ReachableObject prev = obj;
    // FIXME: We only read the first item.
    // assert(set.size() == 1);
    obj = *set.begin();
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

void dump_info(DexStoresVector& stores,
               const ReachableObjectGraph& retainers_of,
               const std::string& dump_tag) {
  for (const auto& dex : DexStoreClassesIterator(stores)) {
    for (const auto& cls : dex) {
      print_reachable_reason(cls, retainers_of, dump_tag);
      print_reachable_stack(cls, retainers_of, dump_tag);
    }
  }
}

void dump_graph(DexStoresVector& stores,
                const ReachableObjectGraph& retainers_of,
                const std::string& dump_tag,
                std::ostream& os) {
  for (const auto& dex : DexStoreClassesIterator(stores)) {
    for (const auto& cls : dex) {
      print_graph_edges(cls, retainers_of, dump_tag, os);
    }
  }
}

} // namespace reachability
