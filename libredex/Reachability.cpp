/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Reachability.h"

#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/range/adaptor/map.hpp>
#include <cinttypes>

#include "AnnotationSignatureParser.h"
#include "BinarySerialization.h"
#include "CFGMutation.h"
#include "DexAnnotation.h"
#include "DexUtil.h"
#include "ProguardConfiguration.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace reachability;

namespace bs = binary_serialization;
namespace mog = method_override_graph;

namespace {

static ReachableObject SEED_SINGLETON{};

} // namespace

namespace reachability {

bool consider_dynamically_referenced(const DexClass* cls) {
  return !root(cls) && !is_interface(cls) && !is_annotation(cls);
}

std::ostream& operator<<(std::ostream& os, const ReachableObject& obj) {
  switch (obj.type) {
  case ReachableObjectType::ANNO:
    return os << show_deobfuscated(obj.anno->type());
  case ReachableObjectType::CLASS:
    return os << show_deobfuscated(obj.cls);
  case ReachableObjectType::FIELD:
    return os << show_deobfuscated(obj.field);
  case ReachableObjectType::METHOD:
    return os << show_deobfuscated(obj.method);
  case ReachableObjectType::SEED: {
    if (obj.keep_reason) {
      return os << *obj.keep_reason;
    }
    return os << "<SEED>";
  }
  }
}

bool RootSetMarker::is_canary(const DexClass* cls) {
  return strstr(cls->get_name()->c_str(), "Canary");
}

bool RootSetMarker::should_mark_cls(const DexClass* cls) {
  return root(cls) || is_canary(cls);
}

void RootSetMarker::mark_all_as_seed(const Scope& scope) {
  walk::parallel::classes(scope, [&](const DexClass* cls) {
    TRACE(REACH, 3, "Visiting seed: %s", SHOW(cls));
    push_seed(cls);

    for (auto const& f : cls->get_ifields()) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
      push_seed(f, Condition::ClassRetained);
    }
    for (auto const& f : cls->get_sfields()) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
      push_seed(f, Condition::ClassRetained);
    }
    for (auto const& m : cls->get_dmethods()) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(m));
      push_seed(m, Condition::ClassRetained);
    }
    for (auto const& m : cls->get_vmethods()) {
      TRACE(REACH, 3, "Visiting seed: %s (root)", SHOW(m));
      push_seed(m, Condition::ClassRetained);
    }
  });
}

bool RootSetMarker::is_rootlike_clinit(const DexMethod* m) {
  return method::is_clinit(m) &&
         (!m->get_code() || !method::is_trivial_clinit(*m->get_code()));
}

bool RootSetMarker::is_rootlike_init(const DexMethod* m) const {
  // We keep the parameterless constructor, in case it's constructed via
  // .class or Class.forName()
  // if m_remove_no_argument_constructors, make an exception. This is only
  // used for testing
  return !m_remove_no_argument_constructors && method::is_argless_init(m);
}

/*
 * Initializes the root set by marking and pushing nodes onto the work queue.
 * Also conditionally marks class member seeds.
 */
void RootSetMarker::mark(const Scope& scope) {
  walk::parallel::classes(scope, [&](const DexClass* cls) {
    if (should_mark_cls(cls)) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(cls));
      push_seed(cls);
    }
    // Applying the same exclusions as DelInitPass
    auto relaxed =
        m_relaxed_keep_class_members && consider_dynamically_referenced(cls);
    // push_seed for an ifield or vmethod
    auto push_iv_seed = [&](auto* m) {
      if (relaxed) {
        push_seed(m, Condition::ClassDynamicallyReferenced);
        push_seed(m, Condition::ClassInstantiable);
      } else {
        push_seed(m, Condition::ClassRetained);
      }
    };
    // push_seed for a dmethod
    auto push_d_seed = [&](auto* m) {
      push_seed(m, (m->get_code() && !method::is_clinit(m) && relaxed)
                       ? Condition::ClassDynamicallyReferenced
                       : Condition::ClassRetained);
    };
    for (auto const& f : cls->get_ifields()) {
      if (root(f)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
        push_iv_seed(f);
      } else if (is_volatile(f) && !m_relaxed_keep_class_members) {
        TRACE(REACH, 3, "Visiting seed (volatile): %s", SHOW(f));
        push_iv_seed(f);
      }
    }
    for (auto const& f : cls->get_sfields()) {
      if (root(f)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
        push_seed(f, Condition::ClassRetained);
      }
    }
    for (auto const& m : cls->get_dmethods()) {
      if (is_rootlike_clinit(m)) {
        TRACE(REACH, 3, "Visiting seed (root-like clinit): %s", SHOW(m));
        push_d_seed(m);
      } else if (is_rootlike_init(m)) {
        TRACE(REACH, 3, "Visiting seed (root-like init): %s", SHOW(m));
        push_d_seed(m);
      } else if (root(m)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(m));
        push_d_seed(m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (root(m)) {
        TRACE(REACH, 3, "Visiting seed: %s (root)", SHOW(m));
        push_iv_seed(m);
      }
    }
  });

  mark_external_method_overriders();
}

void RootSetMarker::mark_with_exclusions(
    const Scope& scope,
    const ConcurrentSet<const DexClass*>& excluded_classes,
    const ConcurrentSet<const DexMethod*>& excluded_methods) {
  auto excluded = [&excluded_classes, &excluded_methods](auto* item) -> bool {
    if constexpr (std::is_same_v<decltype(item), const DexClass*>) {
      return excluded_classes.find(item) != excluded_classes.end();
    } else if constexpr (std::is_same_v<decltype(item), const DexMethod*>) {
      return excluded_methods.find(item) != excluded_methods.end();
    } else {
      static_assert(std::is_same_v<decltype(item), const DexField*>);
      return false;
    }
  };

  walk::parallel::classes(scope, [&](const DexClass* cls) {
    if (should_mark_cls(cls) && !excluded(cls)) {
      TRACE(REACH, 3, "Visiting seed: %s", SHOW(cls));
      push_seed(cls);
    }
    for (const auto* f : cls->get_ifields()) {
      if ((root(f) || is_volatile(f)) && !excluded(f)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
        push_seed(f, Condition::ClassRetained);
      }
    }
    for (const auto* f : cls->get_sfields()) {
      if (root(f) && !excluded(f)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(f));
        push_seed(f, Condition::ClassRetained);
      }
    }
    for (const auto* m : cls->get_dmethods()) {
      if ((root(m) || is_rootlike_clinit(m) || is_rootlike_init(m)) &&
          !excluded(m)) {
        TRACE(REACH, 3, "Visiting seed: %s", SHOW(m));
        push_seed(m, Condition::ClassRetained);
      }
    }
    for (const auto* m : cls->get_vmethods()) {
      if (root(m) && !excluded(m)) {
        TRACE(REACH, 3, "Visiting seed: %s (root)", SHOW(m));
        push_seed(m, Condition::ClassRetained);
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

void RootSetMarker::push_seed(const DexField* field, Condition condition) {
  if (!field) return;
  switch (condition) {
  case Condition::ClassRetained:
    m_cond_marked->if_class_retained.fields.insert(field);
    break;
  case Condition::ClassDynamicallyReferenced:
    m_cond_marked->if_class_dynamically_referenced.fields.insert(field);
    break;
  case Condition::ClassInstantiable:
    m_cond_marked->if_class_instantiable.fields.insert(field);
    break;
  default:
    not_reached();
  }
}

void RootSetMarker::push_seed(const DexMethod* method, Condition condition) {
  if (!method) return;
  switch (condition) {
  case Condition::ClassRetained:
    m_cond_marked->if_class_retained.methods.insert(method);
    break;
  case Condition::ClassDynamicallyReferenced:
    m_cond_marked->if_class_dynamically_referenced.methods.insert(method);
    break;
  case Condition::ClassInstantiable:
    m_cond_marked->if_class_instantiable.methods.insert(method);
    break;
  default:
    not_reached();
  }
}

template <class Seed>
void RootSetMarker::record_is_seed(Seed* seed) {
  if (m_record_reachability) {
    redex_assert(seed != nullptr);
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
        TRACE(REACH, 3, "Visiting seed: %s (implements %s)", SHOW(overriding),
              SHOW(method));
        push_seed(overriding, Condition::ClassInstantiable);
      }
    }
  }
}

/*
 * Marks :obj and pushes its immediately reachable neighbors onto the local
 * task queue of the current worker.
 */
void TransitiveClosureMarkerWorker::visit(const ReachableObject& obj) {
  switch (obj.type) {
  case ReachableObjectType::CLASS:
    visit_cls(obj.cls);
    break;
  case ReachableObjectType::FIELD:
    visit_field_ref(obj.field);
    break;
  case ReachableObjectType::METHOD:
    visit_method_ref(obj.method);
    break;
  case ReachableObjectType::ANNO:
  case ReachableObjectType::SEED:
    not_reached_log("Unexpected ReachableObject type");
  }
}

template <class Parent, class InputIt>
void TransitiveClosureMarkerWorker::push(const Parent* parent,
                                         InputIt begin,
                                         InputIt end) {
  for (auto it = begin; it != end; ++it) {
    push(parent, *it);
  }
}

template <class Parent>
void TransitiveClosureMarkerWorker::push(const Parent* parent,
                                         const DexType* type) {
  type = type::get_element_type_if_array(type);
  push(parent, type_class(type));
}

void TransitiveClosureMarkerWorker::push(const DexMethodRef* parent,
                                         const DexType* type) {
  type = type::get_element_type_if_array(type);
  push(parent, type_class(type));
}

template <class Parent>
void TransitiveClosureMarkerWorker::push(const Parent* parent,
                                         const DexClass* cls) {
  if (!cls) {
    return;
  }
  record_reachability(parent, cls);
  if (!m_shared_state->reachable_objects->mark(cls)) {
    return;
  }
  m_worker_state->push_task(ReachableObject(cls));
}

template <class Parent>
void TransitiveClosureMarkerWorker::push(const Parent* parent,
                                         const DexFieldRef* field) {
  if (!field) {
    return;
  }
  record_reachability(parent, field);
  if (!m_shared_state->reachable_objects->mark(field)) {
    return;
  }
  auto f = field->as_def();
  if (f) {
    gather_and_push(f);
  }
  m_worker_state->push_task(ReachableObject(field));
}

template <class Parent>
void TransitiveClosureMarkerWorker::push(const Parent* parent,
                                         const DexMethodRef* method) {
  if (!method) {
    return;
  }

  record_reachability(parent, method);
  if (!m_shared_state->reachable_objects->mark(method)) {
    return;
  }
  m_worker_state->push_task(ReachableObject(method));
}

void TransitiveClosureMarkerWorker::push(const DexMethodRef* parent,
                                         const DexMethodRef* method) {
  this->template push<DexMethodRef>(parent, method);
}

void TransitiveClosureMarkerWorker::push_if_class_instantiable(
    const DexMethod* method) {
  if (!method || m_shared_state->reachable_objects->marked(method)) return;
  TRACE(REACH, 4,
        "Conditionally marking method if declaring class is instantiable: %s",
        SHOW(method));
  auto clazz = type_class(method->get_class());
  m_shared_state->cond_marked->if_class_instantiable.methods.insert(method);
  // If :clazz is already known to be instantiable, then we cannot count on
  // instantiable(DexClass*) to have moved the
  // conditionally-if-instantiable-marked methods into the actually-marked ones
  // -- we have to do it ourselves. Note that we must do this check after adding
  // :method to m_cond_marked to avoid a race condition where we add to
  // m_cond_marked after instantiable(DexClass*) has finished moving its
  // contents over to m_reachable_objects.
  if (m_shared_state->reachable_aspects->instantiable_types.count(clazz)) {
    push(clazz, method);
  }
}

void TransitiveClosureMarkerWorker::push_if_class_instantiable(
    const DexField* field) {
  if (!field || m_shared_state->reachable_objects->marked(field)) return;
  TRACE(REACH, 4,
        "Conditionally marking field if declaring class is instantiable: %s",
        SHOW(field));
  auto clazz = type_class(field->get_class());
  m_shared_state->cond_marked->if_class_instantiable.fields.insert(field);
  if (m_shared_state->reachable_aspects->instantiable_types.count(clazz)) {
    push(clazz, field);
  }
}

void TransitiveClosureMarkerWorker::push_if_class_instantiable(
    const DexClass* cls,
    std::shared_ptr<MethodReferencesGatherer> method_references_gatherer) {
  always_assert(method_references_gatherer);
  auto method = method_references_gatherer->get_method();
  bool emplaced = false;
  m_shared_state->cond_marked->if_class_instantiable.method_references_gatherers
      .update(cls, [&](auto*, auto& map, bool) {
        auto ptr = method_references_gatherer.get();
        auto p = map.emplace(method, std::move(method_references_gatherer));
        always_assert(ptr == p.first->second.get()); // emplaced or not
        emplaced = p.second;
      });
  always_assert(!method_references_gatherer);
  if (emplaced &&
      m_shared_state->reachable_aspects->instantiable_types.count(cls)) {
    m_shared_state->cond_marked->if_class_instantiable
        .method_references_gatherers.update(cls, [&](auto*, auto& map, bool) {
          auto it = map.find(method);
          if (it != map.end()) {
            method_references_gatherer = std::move(it->second);
            map.erase(it);
          }
        });
    if (method_references_gatherer) {
      gather_and_push(std::move(method_references_gatherer),
                      MethodReferencesGatherer::Advance::instantiable(cls));
    }
  }
}

void TransitiveClosureMarkerWorker::push_if_class_retained(
    const DexMethod* method) {
  if (!method || m_shared_state->reachable_objects->marked(method)) return;
  TRACE(REACH, 4,
        "Conditionally marking method if declaring class is instantiable: %s",
        SHOW(method));
  auto clazz = type_class(method->get_class());
  m_shared_state->cond_marked->if_class_retained.methods.insert(method);
  if (m_shared_state->reachable_objects->marked(clazz)) {
    push(clazz, method);
  }
}

void TransitiveClosureMarkerWorker::push_if_class_retained(
    const DexField* field) {
  if (!field || m_shared_state->reachable_objects->marked(field)) return;
  TRACE(REACH, 4,
        "Conditionally marking field if declaring class is instantiable: %s",
        SHOW(field));
  auto clazz = type_class(field->get_class());
  m_shared_state->cond_marked->if_class_retained.fields.insert(field);
  if (m_shared_state->reachable_objects->marked(clazz)) {
    push(clazz, field);
  }
}

void TransitiveClosureMarkerWorker::
    push_directly_instantiable_if_class_dynamically_referenced(DexType* type) {
  m_shared_state->cond_marked->if_class_dynamically_referenced
      .directly_instantiable_types.insert(type);
  auto clazz = type_class(type);
  if (m_shared_state->reachable_aspects->dynamically_referenced_classes.count(
          clazz)) {
    directly_instantiable(type);
  }
}

void TransitiveClosureMarkerWorker::push_if_instance_method_callable(
    std::shared_ptr<MethodReferencesGatherer> method_references_gatherer) {
  auto* method = method_references_gatherer->get_method();
  m_shared_state->cond_marked->if_instance_method_callable.update(
      method, [&](auto*, auto& value, bool) {
        always_assert(!value);
        value = std::move(method_references_gatherer);
      });
  if (m_shared_state->reachable_aspects->callable_instance_methods.count(
          method)) {
    m_shared_state->cond_marked->if_instance_method_callable.update(
        method, [&](auto*, auto& value, bool) {
          std::swap(method_references_gatherer, value);
        });
    if (method_references_gatherer) {
      gather_and_push(method_references_gatherer,
                      MethodReferencesGatherer::Advance::callable());
    }
  }
}

// Adapted from DelInitPass
namespace relaxed_keep_class_members_impl {

void gather_dynamic_references_impl(const DexAnnotation* anno,
                                    References* references) {
  DexType* dalviksig = type::dalvik_annotation_Signature();
  // Signature annotations contain strings that Jackson uses
  // to construct the underlying types.
  if (anno->type() == dalviksig) {
    annotation_signature_parser::parse(anno, [&](auto*, auto* sigcls) {
      if (sigcls) {
        references->classes_dynamically_referenced.insert(sigcls);
      }
      return true;
    });
    return;
  }
  // Class literals in annotations.
  // Example:
  //    @JsonDeserialize(using=MyJsonDeserializer.class)
  if (anno->runtime_visible()) {
    auto& elems = anno->anno_elems();
    std::vector<DexType*> ltype;
    for (auto const& dae : elems) {
      auto& evalue = dae.encoded_value;
      evalue->gather_types(ltype);
    }
    for (auto dextype : ltype) {
      auto cls = type_class(dextype);
      if (cls) {
        references->classes_dynamically_referenced.insert(cls);
      }
    }
  }
}

void gather_dynamic_references_anno_set(const DexAnnotationSet* anno_set,
                                        References* references) {
  if (anno_set) {
    for (auto& anno : anno_set->get_annotations()) {
      gather_dynamic_references_impl(anno.get(), references);
    }
  }
}

template <class T>
void gather_dynamic_references(T item, References* references) {
  gather_dynamic_references_anno_set(item->get_anno_set(), references);
}

template <>
void gather_dynamic_references(const DexMethod* item, References* references) {
  gather_dynamic_references_anno_set(item->get_anno_set(), references);
  auto param_anno = item->get_param_anno();
  if (param_anno) {
    for (auto&& [_, param_anno_set] : *param_anno) {
      gather_dynamic_references_anno_set(param_anno_set.get(), references);
    }
  }
}

template <>
void gather_dynamic_references(const DexAnnotation* item,
                               References* references) {
  gather_dynamic_references_impl(item, references);
}

// Note: this method will return nullptr if the dotname refers to an unknown
// type.
DexType* get_dextype_from_dotname(std::string_view dotname) {
  std::string buf;
  buf.reserve(dotname.size() + 2);
  buf += 'L';
  buf += dotname;
  buf += ';';
  std::replace(buf.begin(), buf.end(), '.', '/');
  return DexType::get_type(buf);
}

template <>
void gather_dynamic_references(const MethodItemEntry* item,
                               References* references) {
  if (item->type != MFLOW_OPCODE) {
    return;
  }
  auto insn = item->insn;
  // Matches any stringref that name-aliases a type.
  if (insn->has_string()) {
    const DexString* dsclzref = insn->get_string();
    auto* cls = type_class(get_dextype_from_dotname(dsclzref->str()));
    if (cls) {
      references->classes_dynamically_referenced.insert(cls);
    }
  }
  if (opcode::is_new_instance(insn->opcode()) ||
      opcode::is_const_class(insn->opcode())) {
    auto* cls = type_class(insn->get_type());
    if (cls) {
      references->classes_dynamically_referenced.insert(cls);
    }
  }
}

} // namespace relaxed_keep_class_members_impl

MethodReferencesGatherer::MethodReferencesGatherer(
    const DexMethod* method,
    bool include_dynamic_references,
    bool check_init_instantiable,
    std::function<bool(const DexClass*)> is_class_instantiable,
    bool consider_code,
    GatherMieFunction gather_mie)
    : m_method(method),
      m_include_dynamic_references(include_dynamic_references),
      m_check_init_instantiable(check_init_instantiable),
      m_is_class_instantiable(std::move(is_class_instantiable)),
      m_consider_code(consider_code),
      m_gather_mie(gather_mie ? std::move(gather_mie)
                              : std::bind(default_gather_mie,
                                          include_dynamic_references,
                                          std::placeholders::_1,
                                          std::placeholders::_2,
                                          std::placeholders::_3,
                                          /* gather_methods */ true)) {}

std::optional<MethodReferencesGatherer::InstantiableDependency>
MethodReferencesGatherer::get_instantiable_dependency(
    const MethodItemEntry& mie) const {
  if (mie.type != MFLOW_OPCODE) {
    return std::nullopt;
  }
  auto insn = mie.insn;
  auto op = insn->opcode();
  InstantiableDependency res;
  if (opcode::is_an_ifield_op(op)) {
    res.cls = type_class(insn->get_field()->get_class());
    res.may_continue_normally_if_uninstantiable = false;
  } else if (opcode::is_invoke_virtual(op) || opcode::is_invoke_super(op) ||
             opcode::is_invoke_interface(op) ||
             (opcode::is_invoke_direct(op) &&
              (m_check_init_instantiable ||
               !method::is_init(insn->get_method())))) {
    res.cls = type_class(insn->get_method()->get_class());
    res.may_continue_normally_if_uninstantiable = false;
  } else if (opcode::is_instance_of(op)) {
    res.cls = type_class(insn->get_type());
    res.may_throw_if_uninstantiable = false;
  } else if (opcode::is_check_cast(op)) {
    res.cls = type_class(insn->get_type());
  }
  if (!res.cls || m_is_class_instantiable(res.cls) ||
      (res.cls->is_external() && !type::is_void(res.cls->get_type()))) {
    return std::nullopt;
  }
  return res;
};

void MethodReferencesGatherer::default_gather_mie(
    bool include_dynamic_references,
    const DexMethod* method,
    const MethodItemEntry& mie,
    References* refs,
    bool gather_methods) {
  mie.gather_strings(refs->strings);
  mie.gather_types(refs->types);
  mie.gather_fields(refs->fields);
  if (gather_methods) {
    mie.gather_methods(refs->methods);
  }
  if (include_dynamic_references) {
    relaxed_keep_class_members_impl::gather_dynamic_references(&mie, refs);
  }
  if (mie.type == MFLOW_OPCODE) {
    if (opcode::is_new_instance(mie.insn->opcode())) {
      refs->new_instances.push_back(mie.insn->get_type());
    } else if (opcode::is_invoke_super(mie.insn->opcode())) {
      auto callee =
          resolve_method(mie.insn->get_method(), MethodSearch::Super, method);
      if (callee && !callee->is_external()) {
        refs->called_super_methods.push_back(callee);
      }
    }
  }
}

void MethodReferencesGatherer::advance(const Advance& advance,
                                       References* refs) {
  always_assert(advance.kind() == m_next_advance_kind);
  if (advance.kind() == AdvanceKind::Initial) {
    // initial gathering
    m_method->gather_types_shallow(refs->types); // Handle DexMethodRef parts.
    auto gather_from_anno_set = [refs](auto* anno_set) {
      anno_set->gather_strings(refs->strings);
      anno_set->gather_types(refs->types);
      anno_set->gather_fields(refs->fields);
      anno_set->gather_methods(refs->methods);
    };
    auto anno_set = m_method->get_anno_set();
    if (anno_set) {
      gather_from_anno_set(anno_set);
    }
    auto param_anno = m_method->get_param_anno();
    if (param_anno) {
      for (auto&& [_, param_anno_set] : *param_anno) {
        gather_from_anno_set(param_anno_set.get());
      }
    }
    if (m_include_dynamic_references) {
      relaxed_keep_class_members_impl::gather_dynamic_references(m_method,
                                                                 refs);
    }
    refs->method_references_gatherer_dependency_if_instance_method_callable =
        true;
    m_next_advance_kind = AdvanceKind::Callable;
    return;
  }
  std::lock_guard<std::mutex> lock_guard(m_mutex);
  std::queue<CFGNeedle> queue;
  if (advance.kind() == AdvanceKind::Callable) {
    std::vector<CFGNeedle> cfg_needles;
    auto code = m_method->get_code();
    if (code && m_consider_code) {
      always_assert_log(code->editable_cfg_built(),
                        "%s does not have editable cfg", SHOW(m_method));
      auto& cfg = code->cfg();
      auto b = cfg.entry_block();
      queue.push((CFGNeedle){b, b->begin()});
      m_pushed_blocks.insert(b);
    }
    m_next_advance_kind = AdvanceKind::InstantiableDependencyResolved;
  } else {
    always_assert(advance.kind() ==
                  AdvanceKind::InstantiableDependencyResolved);
    auto it = m_instantiable_dependencies.find(advance.instantiable_cls());
    if (it == m_instantiable_dependencies.end()) {
      return;
    }
    for (auto& cfg_needle : it->second) {
      queue.push(cfg_needle);
    }
    m_instantiable_dependencies.erase(it);
  }
  auto advance_in_block = [this, refs](auto* block, auto& it) {
    for (; it != block->end(); ++it) {
      auto dep = get_instantiable_dependency(*it);
      if (dep) {
        return dep;
      }
      m_gather_mie(m_method, *it, refs);
      if (it->type == MFLOW_OPCODE) {
        m_instructions_visited++;
      }
    }
    return std::optional<InstantiableDependency>();
  };
  auto visit_succs = [this, &refs, &queue](auto* block, const auto& pred) {
    for (auto* e : block->succs()) {
      if (!pred(e)) {
        continue;
      }
      if (e->type() == cfg::EDGE_THROW) {
        auto catch_type = e->throw_info()->catch_type;
        if (catch_type && m_covered_catch_types.insert(catch_type).second) {
          refs->types.push_back(catch_type);
        }
      }
      if (m_pushed_blocks.insert(e->target()).second) {
        queue.push((CFGNeedle){e->target(), e->target()->begin()});
      }
    }
  };
  while (!queue.empty()) {
    auto [block, it] = queue.front();
    queue.pop();
    auto dep = advance_in_block(block, it);
    if (!dep) {
      always_assert(it == block->end());
      visit_succs(block, [](auto*) { return true; });
      continue;
    }
    always_assert(it->type == MFLOW_OPCODE);
    if (!dep->may_continue_normally_if_uninstantiable) {
      auto [deps_it, emplaced] = m_instantiable_dependencies.emplace(
          dep->cls, std::vector<CFGNeedle>());
      if (emplaced) {
        refs->method_references_gatherer_dependencies_if_class_instantiable
            .push_back(dep->cls);
      }
      deps_it->second.push_back((CFGNeedle){block, it});
      always_assert(dep->may_throw_if_uninstantiable);
      if (block->get_last_insn() == it) {
        visit_succs(block,
                    [](auto* e) { return e->type() == cfg::EDGE_THROW; });
      }
      continue;
    }
    m_instructions_visited++;
    queue.push((CFGNeedle){block, std::next(it)});
  }
}

void gather_dynamic_references(const DexAnnotation* item,
                               References* references) {
  relaxed_keep_class_members_impl::gather_dynamic_references(item, references);
}

void gather_dynamic_references(const MethodItemEntry* mie,
                               References* references) {
  relaxed_keep_class_members_impl::gather_dynamic_references(mie, references);
}

template <class T>
static References generic_gather(T t, bool include_dynamic_references) {
  References refs;
  t->gather_strings(refs.strings);
  t->gather_types(refs.types);
  t->gather_fields(refs.fields);
  t->gather_methods(refs.methods);
  if (include_dynamic_references) {
    relaxed_keep_class_members_impl::gather_dynamic_references<T>(t, &refs);
  }
  return refs;
}

References TransitiveClosureMarkerWorker::gather(
    const DexAnnotation* anno) const {
  return generic_gather(anno, m_shared_state->relaxed_keep_class_members);
}

References TransitiveClosureMarkerWorker::gather(const DexField* field) const {
  return generic_gather(field, m_shared_state->relaxed_keep_class_members);
}

bool TransitiveClosureMarkerWorker::has_class_forName(const DexMethod* meth) {
  auto code = meth->get_code();
  auto* class_forName = method::java_lang_Class_forName();
  if (!code || !class_forName) {
    return false;
  }
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  for (auto& mie : InstructionIterable(cfg)) {
    auto insn = mie.insn;
    if (insn->has_method() && insn->get_method() == class_forName) {
      return true;
    }
  }
  return false;
}

void TransitiveClosureMarkerWorker::gather_and_push(
    std::shared_ptr<MethodReferencesGatherer> method_references_gatherer,
    const MethodReferencesGatherer::Advance& advance) {
  always_assert(method_references_gatherer);
  References refs;
  method_references_gatherer->advance(advance, &refs);
  auto* meth = method_references_gatherer->get_method();
  if (refs.method_references_gatherer_dependency_if_instance_method_callable &&
      (!m_shared_state->cfg_gathering_check_instantiable ||
       !m_shared_state->cfg_gathering_check_instance_callable ||
       meth->rstate.no_optimizations() || is_static(meth) ||
       m_shared_state->reachable_aspects->callable_instance_methods.count(
           meth))) {
    always_assert(advance.kind() ==
                  MethodReferencesGatherer::AdvanceKind::Initial);
    refs.method_references_gatherer_dependency_if_instance_method_callable =
        false;
    method_references_gatherer->advance(
        MethodReferencesGatherer::Advance::callable(), &refs);
    always_assert(
        !refs.method_references_gatherer_dependency_if_instance_method_callable);
  }
  auto* type = meth->get_class();
  auto* cls = type_class(type);
  bool check_strings = m_shared_state->ignore_sets->keep_class_in_string;
  if (!check_strings && !refs.strings.empty() && has_class_forName(meth)) {
    check_strings = true;
  }
  if (m_shared_state->ignore_sets->string_literals.count(type)) {
    ++m_shared_state->stats->num_ignore_check_strings;
    check_strings = false;
  }
  if (cls && check_strings) {
    for (const auto& ignore_anno_type :
         m_shared_state->ignore_sets->string_literal_annos) {
      if (has_anno(cls, ignore_anno_type)) {
        ++m_shared_state->stats->num_ignore_check_strings;
        check_strings = false;
        break;
      }
    }
  }
  if (check_strings) {
    push_typelike_strings(meth, refs.strings);
  }
  push(meth, refs.types.begin(), refs.types.end());
  push(meth, refs.fields.begin(), refs.fields.end());
  push(meth, refs.methods.begin(), refs.methods.end());
  for (auto* vmeth : refs.vmethods_if_class_instantiable) {
    push_if_class_instantiable(vmeth);
  }
  dynamically_referenced(refs.classes_dynamically_referenced);
  directly_instantiable(refs.new_instances);
  instance_callable(refs.called_super_methods);
  if (refs.method_references_gatherer_dependency_if_instance_method_callable) {
    push_if_instance_method_callable(method_references_gatherer);
    always_assert(
        refs.method_references_gatherer_dependencies_if_class_instantiable
            .empty());
    return;
  }
  auto& v = refs.method_references_gatherer_dependencies_if_class_instantiable;
  if (v.empty()) {
    return;
  }
  for (size_t i = 0; i < v.size() - 1; ++i) {
    push_if_class_instantiable(v[i], method_references_gatherer);
  }
  push_if_class_instantiable(v[v.size() - 1],
                             std::move(method_references_gatherer));
}

void TransitiveClosureMarkerWorker::gather_and_push(const DexMethod* meth) {
  gather_and_push(create_method_references_gatherer(meth),
                  MethodReferencesGatherer::Advance::initial());
}

std::shared_ptr<MethodReferencesGatherer>
TransitiveClosureMarkerWorker::create_method_references_gatherer(
    const DexMethod* method, bool consider_code, GatherMieFunction gather_mie) {
  const auto* instantiable_types =
      (m_shared_state->cfg_gathering_check_instantiable &&
       !method->rstate.no_optimizations())
          ? &m_shared_state->reachable_aspects->instantiable_types
          : nullptr;
  auto is_class_instantiable = [instantiable_types](const auto* cls) {
    return !instantiable_types || instantiable_types->count(cls);
  };
  return std::make_shared<MethodReferencesGatherer>(
      method, m_shared_state->relaxed_keep_class_members,
      m_shared_state->cfg_gathering_check_instance_callable,
      std::move(is_class_instantiable), consider_code, std::move(gather_mie));
}

template <typename T>
void TransitiveClosureMarkerWorker::gather_and_push(T t) {
  auto refs = gather(t);
  push_typelike_strings(t, refs.strings);
  push(t, refs.types.begin(), refs.types.end());
  push(t, refs.fields.begin(), refs.fields.end());
  push(t, refs.methods.begin(), refs.methods.end());
  dynamically_referenced(refs.classes_dynamically_referenced);
  always_assert(refs.new_instances.empty());
  always_assert(refs.called_super_methods.empty());
}

template <class Parent>
void TransitiveClosureMarkerWorker::push_typelike_strings(
    const Parent* parent, const std::vector<const DexString*>& strings) {
  for (auto const& str : strings) {
    auto internal = java_names::external_to_internal(str->str());
    auto type = DexType::get_type(internal);
    if (!type) {
      continue;
    }
    push(parent, type);
  }
}

void TransitiveClosureMarkerWorker::visit_cls(const DexClass* cls) {
  TRACE(REACH, 4, "Visiting class: %s", SHOW(cls));
  auto is_interface_instantiable = [](const DexClass* interface) {
    if (is_annotation(interface) || root(interface) || !can_rename(interface)) {
      return true;
    }
    for (auto method : interface->get_vmethods()) {
      if (root(method) || !can_rename(method)) {
        return true;
      }
    }
    return false;
  };
  if (is_interface(cls) && is_interface_instantiable(cls)) {
    instantiable(cls->get_type());
  }
  push(cls, type_class(cls->get_super_class()));
  for (auto const& t : *cls->get_interfaces()) {
    push(cls, t);
  }
  const DexAnnotationSet* annoset = cls->get_anno_set();
  if (annoset) {
    for (auto const& anno : annoset->get_annotations()) {
      if (m_shared_state->ignore_sets->system_annos.count(anno->type())) {
        TRACE(REACH,
              5,
              "Stop marking from %s by system anno: %s",
              SHOW(cls),
              SHOW(anno->type()));
        if (m_shared_state->relaxed_keep_class_members) {
          References refs;
          gather_dynamic_references(anno.get(), &refs);
          dynamically_referenced(refs.classes_dynamically_referenced);
        }
        continue;
      }
      record_reachability(cls, anno.get());
      gather_and_push(anno.get());
    }
  }

  if (m_shared_state->relaxed_keep_class_members &&
      consider_dynamically_referenced(cls) && marked_by_string(cls)) {
    dynamically_referenced(cls);
  }

  auto* cond_marked = m_shared_state->cond_marked;
  for (auto const& m : cls->get_ifields()) {
    if (cond_marked->if_class_retained.fields.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_sfields()) {
    if (cond_marked->if_class_retained.fields.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_dmethods()) {
    if (cond_marked->if_class_retained.methods.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_vmethods()) {
    if (cond_marked->if_class_retained.methods.count(m)) {
      push(cls, m);
    }
  }
}

void TransitiveClosureMarkerWorker::visit_field_ref(const DexFieldRef* field) {
  TRACE(REACH, 4, "Visiting field: %s", SHOW(field));
  if (!field->is_concrete()) {
    auto const& realfield =
        resolve_field(field->get_class(), field->get_name(), field->get_type());
    push(field, realfield);
  }
  push(field, field->get_class());
  push(field, field->get_type());
}

const DexMethod* resolve_without_context(const DexMethodRef* method,
                                         const DexClass* cls) {
  if (!cls) return nullptr;
  for (auto const& m : cls->get_vmethods()) {
    if (method::signatures_match(method, m)) {
      return m;
    }
  }
  for (auto const& m : cls->get_dmethods()) {
    if (method::signatures_match(method, m)) {
      return m;
    }
  }
  {
    auto const& superclass = type_class(cls->get_super_class());
    auto const resolved = resolve_without_context(method, superclass);
    if (resolved) {
      return resolved;
    }
  }
  for (auto const& interface : *cls->get_interfaces()) {
    auto const resolved =
        resolve_without_context(method, type_class(interface));
    if (resolved) {
      return resolved;
    }
  }
  return nullptr;
}

void TransitiveClosureMarkerWorker::instantiable(DexType* type) {
  auto cls = type_class(type);
  if (!cls || cls->is_external()) {
    return;
  }
  if (!m_shared_state->reachable_aspects->instantiable_types.insert(cls)) {
    return;
  }
  instantiable(cls->get_super_class());
  for (auto* intf : *cls->get_interfaces()) {
    instantiable(intf);
  }
  auto* cond_marked = m_shared_state->cond_marked;
  for (auto const& f : cls->get_ifields()) {
    if (cond_marked->if_class_instantiable.fields.count(f)) {
      push(cls, f);
    }
  }
  for (auto const& m : cls->get_dmethods()) {
    if (cond_marked->if_class_instantiable.methods.count(m)) {
      push(cls, m);
    }
  }
  for (auto const& m : cls->get_vmethods()) {
    if (cond_marked->if_class_instantiable.methods.count(m)) {
      push(cls, m);
    }
  }
  MethodReferencesGatherers method_references_gatherers;
  cond_marked->if_class_instantiable.method_references_gatherers.update(
      cls, [&](auto*, auto& map, bool) {
        method_references_gatherers = std::move(map);
      });
  for (auto&& [_, method_references_gatherer] : method_references_gatherers) {
    gather_and_push(std::move(method_references_gatherer),
                    MethodReferencesGatherer::Advance::instantiable(cls));
  }
}

void TransitiveClosureMarkerWorker::directly_instantiable(DexType* type) {
  if (m_shared_state->cfg_gathering_check_instance_callable) {
    instantiable(type);
  }
  std::unordered_set<const DexMethod*> uncallable_vmethods;
  auto& method_override_graph = *m_shared_state->method_override_graph;
  for (auto cls = type_class(type); cls && !cls->is_external();
       cls = type_class(cls->get_super_class())) {
    for (auto* m : cls->get_dmethods()) {
      if (!is_static(m)) {
        instance_callable(m);
      }
    }
    for (auto* m : cls->get_vmethods()) {
      if (uncallable_vmethods.count(m)) {
        continue;
      }
      instance_callable(m);
      const auto& overridden_methods =
          mog::get_overridden_methods(method_override_graph, m);
      uncallable_vmethods.insert(overridden_methods.begin(),
                                 overridden_methods.end());
    }
  }
}

void TransitiveClosureMarkerWorker::instance_callable(DexMethod* method) {
  if (!m_shared_state->reachable_aspects->callable_instance_methods.insert(
          method)) {
    return;
  }
  std::shared_ptr<MethodReferencesGatherer> method_references_gatherer;
  m_shared_state->cond_marked->if_instance_method_callable.update(
      method, [&](auto*, auto& value, bool) {
        std::swap(method_references_gatherer, value);
      });
  if (method_references_gatherer) {
    gather_and_push(method_references_gatherer,
                    MethodReferencesGatherer::Advance::callable());
  }
}

void TransitiveClosureMarkerWorker::dynamically_referenced(
    const DexClass* cls) {
  always_assert(m_shared_state->relaxed_keep_class_members);
  if (!consider_dynamically_referenced(cls) ||
      !m_shared_state->reachable_aspects->dynamically_referenced_classes.insert(
          cls)) {
    return;
  }
  auto* cond_marked = m_shared_state->cond_marked;
  for (auto const& f : cls->get_ifields()) {
    if (cond_marked->if_class_dynamically_referenced.fields.count(f)) {
      push_if_class_retained(f);
    }
  }
  for (auto const& f : cls->get_sfields()) {
    if (cond_marked->if_class_dynamically_referenced.fields.count(f)) {
      push_if_class_retained(f);
    }
  }
  for (auto const& m : cls->get_dmethods()) {
    if (cond_marked->if_class_dynamically_referenced.methods.count(m)) {
      push_if_class_retained(m);
    }
  }
  for (auto const& m : cls->get_vmethods()) {
    if (cond_marked->if_class_dynamically_referenced.methods.count(m)) {
      push_if_class_retained(m);
    }
  }
  auto type = cls->get_type();
  if (cond_marked->if_class_dynamically_referenced.directly_instantiable_types
          .count(type)) {
    directly_instantiable(type);
  }
}

void TransitiveClosureMarkerWorker::visit_method_ref(
    const DexMethodRef* method) {
  TRACE(REACH, 4, "Visiting method: %s", SHOW(method));
  auto cls = type_class(method->get_class());
  auto resolved_method = resolve_without_context(method, cls);
  if (resolved_method != nullptr) {
    TRACE(REACH, 5, "    Resolved to: %s", SHOW(resolved_method));
    push(method, resolved_method);
    if (resolved_method == method) {
      gather_and_push(resolved_method);
    }
  }
  push(method, method->get_class());
  push(method, method->get_proto()->get_rtype());
  for (auto const& t : *method->get_proto()->get_args()) {
    push(method, t);
  }
  if (cls && !is_abstract(cls) && method::is_init(method)) {
    if (!m_shared_state->cfg_gathering_check_instance_callable) {
      instantiable(method->get_class());
    }
    if (m_shared_state->relaxed_keep_class_members &&
        consider_dynamically_referenced(cls)) {
      push_directly_instantiable_if_class_dynamically_referenced(
          method->get_class());
    } else {
      directly_instantiable(method->get_class());
    }
  }
  auto m = method->as_def();
  if (!m) {
    return;
  }
  // If we're keeping an interface or virtual method, we have to keep its
  // implementations and overriding methods respectively.
  if (m->is_virtual() || !m->is_concrete()) {
    const auto& overriding_methods =
        mog::get_overriding_methods(*m_shared_state->method_override_graph, m);
    for (auto* overriding : overriding_methods) {
      push_if_class_instantiable(overriding);
    }
  }
}

template <class Parent, class Object>
void TransitiveClosureMarkerWorker::record_reachability(Parent* parent,
                                                        Object* object) {
  if (m_shared_state->record_reachability) {
    redex_assert(parent != nullptr && object != nullptr);
    m_shared_state->reachable_objects->record_reachability(parent, object);
  }
}

void ReachableAspects::finish(const ConditionallyMarked& cond_marked) {
  always_assert(
      cond_marked.if_class_retained.method_references_gatherers.empty());
  always_assert(cond_marked.if_class_dynamically_referenced
                    .method_references_gatherers.empty());
  std::unordered_map<const DexMethod*, const MethodReferencesGatherer*>
      remaining_method_references_gatherers;
  for (auto&& [cls, map] :
       cond_marked.if_class_instantiable.method_references_gatherers) {
    if (map.empty()) {
      continue;
    }
    always_assert(!instantiable_types.count(cls));
    uninstantiable_dependencies.insert(cls);
    for (auto&& [method, method_references_gatherer] : map) {
      remaining_method_references_gatherers.emplace(
          method, method_references_gatherer.get());
    }
  }
  std::atomic<uint64_t> concurrent_instructions_unvisited{0};
  workqueue_run<std::pair<const DexMethod*, const MethodReferencesGatherer*>>(
      [&](std::pair<const DexMethod*, const MethodReferencesGatherer*> p) {
        auto ii = InstructionIterable(p.first->get_code()->cfg());
        auto size = std::distance(ii.begin(), ii.end());
        auto visited = p.second->get_instructions_visited();
        always_assert(visited <= size);
        concurrent_instructions_unvisited += size - visited;
      },
      remaining_method_references_gatherers);
  instructions_unvisited = (uint64_t)concurrent_instructions_unvisited;
  TRACE(RMU, 1,
        "%zu uninstantiable_dependencies, %" PRIu64 " instructions_unvisited",
        uninstantiable_dependencies.size(), instructions_unvisited);
}

std::unique_ptr<ReachableObjects> compute_reachable_objects(
    const DexStoresVector& stores,
    const IgnoreSets& ignore_sets,
    int* num_ignore_check_strings,
    ReachableAspects* reachable_aspects,
    bool record_reachability,
    bool relaxed_keep_class_members,
    bool cfg_gathering_check_instantiable,
    bool cfg_gathering_check_instance_callable,
    bool should_mark_all_as_seed,
    std::unique_ptr<const mog::Graph>* out_method_override_graph,
    bool remove_no_argument_constructors) {
  Timer t("Marking");
  auto scope = build_class_scope(stores);
  auto reachable_objects = std::make_unique<ReachableObjects>();
  ConditionallyMarked cond_marked;
  auto method_override_graph = mog::build_graph(scope);

  ConcurrentSet<ReachableObject, ReachableObjectHash> root_set;
  RootSetMarker root_set_marker(*method_override_graph, record_reachability,
                                relaxed_keep_class_members,
                                remove_no_argument_constructors, &cond_marked,
                                reachable_objects.get(), &root_set);

  if (should_mark_all_as_seed) {
    root_set_marker.mark_all_as_seed(scope);
  } else {
    root_set_marker.mark(scope);
  }

  size_t num_threads = redex_parallel::default_num_threads();
  Stats stats;
  TransitiveClosureMarkerSharedState shared_state{
      &ignore_sets,
      method_override_graph.get(),
      record_reachability,
      relaxed_keep_class_members,
      cfg_gathering_check_instantiable,
      cfg_gathering_check_instance_callable,
      &cond_marked,
      reachable_objects.get(),
      reachable_aspects,
      &stats};
  workqueue_run<ReachableObject>(
      [&](TransitiveClosureMarkerWorkerState* worker_state,
          const ReachableObject& obj) {
        TransitiveClosureMarkerWorker worker(&shared_state, worker_state);
        worker.visit(obj);
        return nullptr;
      },
      root_set,
      num_threads,
      /*push_tasks_while_running=*/true);

  if (num_ignore_check_strings != nullptr) {
    *num_ignore_check_strings = (int)stats.num_ignore_check_strings;
  }

  if (out_method_override_graph) {
    *out_method_override_graph = std::move(method_override_graph);
  }

  reachable_aspects->finish(cond_marked);

  return reachable_objects;
}

void ReachableObjects::record_reachability(const DexMethodRef* member,
                                           const DexClass* cls) {
  // Each class member trivially retains its containing class; let's filter out
  // this uninteresting information from our diagnostics.
  if (member->get_class() == cls->get_type()) {
    return;
  }
  m_retainers_of.update(ReachableObject(cls),
                        [&](const ReachableObject&, ReachableObjectSet& set,
                            bool /* exists */) { set.emplace(member); });
}

void ReachableObjects::record_reachability(const DexFieldRef* member,
                                           const DexClass* cls) {
  if (member->get_class() == cls->get_type()) {
    return;
  }
  m_retainers_of.update(ReachableObject(cls),
                        [&](const ReachableObject&, ReachableObjectSet& set,
                            bool /* exists */) { set.emplace(member); });
}

template <class Object>
void ReachableObjects::record_reachability(Object* parent, Object* object) {
  if (parent == object) {
    return;
  }
  m_retainers_of.update(ReachableObject(object),
                        [&](const ReachableObject&, ReachableObjectSet& set,
                            bool /* exists */) { set.emplace(parent); });
}

template <class Parent, class Object>
void ReachableObjects::record_reachability(Parent* parent, Object* object) {
  m_retainers_of.update(ReachableObject(object),
                        [&](const ReachableObject&, ReachableObjectSet& set,
                            bool /* exists */) { set.emplace(parent); });
}

template <class Seed>
void ReachableObjects::record_is_seed(Seed* seed) {
  redex_assert(seed != nullptr);
  const auto& keep_reasons = seed->rstate.keep_reasons();
  m_retainers_of.update(
      ReachableObject(seed),
      [&](const ReachableObject&, ReachableObjectSet& set, bool /* exists */) {
        for (const auto& reason : keep_reasons) {
          // -keepnames rules are irrelevant when analyzing reachability
          if (reason->type == keep_reason::KEEP_RULE &&
              reason->keep_rule->allowshrinking) {
            continue;
          }
          set.emplace(reason);
        }
      });
}

/*
 * Remove unmarked classes / methods / fields. and add all swept objects to
 * :removed_symbols.
 */
template <class Container, typename EraseHookFn>
static void sweep_if_unmarked(const ReachableObjects& reachables,
                              EraseHookFn erase_hook,
                              Container* c,
                              ConcurrentSet<std::string>* removed_symbols) {
  auto p = [&](const auto& m) {
    if (reachables.marked_unsafe(m) == 0) {
      TRACE(RMU, 2, "Removing %s", SHOW(m));
      return false;
    }
    return true;
  };
  const auto it = std::partition(c->begin(), c->end(), p);
  if (removed_symbols) {
    for (auto i = it; i != c->end(); i++) {
      removed_symbols->insert(show_deobfuscated(*i));
      erase_hook(*i);
    }
  } else {
    for (auto i = it; i != c->end(); i++) {
      erase_hook(*i);
    }
  }
  c->erase(it, c->end());
}

void sweep(DexStoresVector& stores,
           const ReachableObjects& reachables,
           ConcurrentSet<std::string>* removed_symbols,
           bool output_full_removed_symbols) {
  Timer t("Sweep");
  auto scope = build_class_scope(stores);

  std::unordered_set<DexClass*> sweeped_classes;
  for (auto& dex : DexStoreClassesIterator(stores)) {
    sweep_if_unmarked(
        reachables, [&](auto cls) { sweeped_classes.insert(cls); }, &dex,
        removed_symbols);
  }

  auto sweep_method = [&](DexMethodRef* m) {
    DexMethod::erase_method(m);
    if (m->is_def()) {
      DexMethod::delete_method(m->as_def());
    }
  };

  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (sweeped_classes.count(cls)) {
      for (auto field : cls->get_all_fields()) {
        DexField::delete_field_DO_NOT_USE(field);
      }
      cls->get_ifields().clear();
      cls->get_sfields().clear();
      for (auto method : cls->get_all_methods()) {
        if (removed_symbols && output_full_removed_symbols) {
          removed_symbols->insert(show_deobfuscated(method));
        }
        sweep_method(method);
      }
      cls->get_dmethods().clear();
      cls->get_vmethods().clear();
      return;
    }
    sweep_if_unmarked(reachables, DexField::delete_field_DO_NOT_USE,
                      &cls->get_ifields(), removed_symbols);
    sweep_if_unmarked(reachables, DexField::delete_field_DO_NOT_USE,
                      &cls->get_sfields(), removed_symbols);
    sweep_if_unmarked(reachables, sweep_method, &cls->get_dmethods(),
                      removed_symbols);
    sweep_if_unmarked(reachables, sweep_method, &cls->get_vmethods(),
                      removed_symbols);
  });
}

remove_uninstantiables_impl::Stats sweep_code(
    DexStoresVector& stores,
    bool prune_uncallable_instance_method_bodies,
    bool skip_uncallable_virtual_methods,
    const ReachableAspects& reachable_aspects) {
  Timer t("Sweep Code");
  auto scope = build_class_scope(stores);
  std::unordered_set<DexType*> uninstantiable_types;
  std::unordered_set<DexMethod*> uncallable_instance_methods;
  for (auto* cls : scope) {
    if (!reachable_aspects.instantiable_types.count_unsafe(cls)) {
      uninstantiable_types.insert(cls->get_type());
    }
    if (prune_uncallable_instance_method_bodies) {
      for (auto* m : cls->get_dmethods()) {
        if (!is_static(m) &&
            !reachable_aspects.callable_instance_methods.count_unsafe(m)) {
          uncallable_instance_methods.insert(m);
        }
      }
      for (auto* m : cls->get_vmethods()) {
        if (!reachable_aspects.callable_instance_methods.count_unsafe(m)) {
          uncallable_instance_methods.insert(m);
        }
      }
    }
  }
  uninstantiable_types.insert(type::java_lang_Void());
  return walk::parallel::methods<remove_uninstantiables_impl::Stats>(
      scope, [&](DexMethod* method) {
        auto code = method->get_code();
        if (!code || method->rstate.no_optimizations()) {
          return remove_uninstantiables_impl::Stats();
        }
        always_assert(code->editable_cfg_built());
        auto& cfg = code->cfg();
        if (uncallable_instance_methods.count(method)) {
          if (skip_uncallable_virtual_methods && method->is_virtual()) {
            return remove_uninstantiables_impl::Stats();
          }
          return remove_uninstantiables_impl::replace_all_with_throw(cfg);
        }
        auto stats = remove_uninstantiables_impl::replace_uninstantiable_refs(
            uninstantiable_types, cfg);
        cfg.remove_unreachable_blocks();
        return stats;
      });
}

remove_uninstantiables_impl::Stats sweep_uncallable_virtual_methods(
    DexStoresVector& stores, const ReachableAspects& reachable_aspects) {
  Timer t("Sweep Uncallable Virtual Methods");
  auto scope = build_class_scope(stores);
  std::unordered_set<DexMethod*> uncallable_instance_methods;
  for (auto* cls : scope) {
    if (is_interface(cls)) {
      // TODO: Is this needed?
      continue;
    }
    walk::methods((Scope){cls}, [&](DexMethod* m) {
      if (is_static(m)) {
        return;
      }
      if (!m->rstate.no_optimizations() && m->get_code() &&
          !reachable_aspects.callable_instance_methods.count_unsafe(m)) {
        uncallable_instance_methods.insert(m);
      }
    });
  }
  return remove_uninstantiables_impl::reduce_uncallable_instance_methods(
      scope, uncallable_instance_methods);
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

// Graph serialization helpers
namespace {

void write_reachable_object(std::ostream& os, const ReachableObject& obj) {
  bs::write<uint8_t>(os, static_cast<uint8_t>(obj.type));
  std::ostringstream ss;
  ss << obj;
  const auto& s = ss.str();
  bs::write<uint32_t>(os, s.size());
  os << s;
}

} // namespace

void dump_graph(std::ostream& os, const ReachableObjectGraph& retainers_of) {
  auto compare = [](const ReachableObject& lhs, const ReachableObject& rhs) {
    if (lhs.type != rhs.type) {
      return lhs.type < rhs.type;
    }
    switch (lhs.type) {
    case ReachableObjectType::CLASS:
      return compare_dexclasses(lhs.cls, rhs.cls);
    case ReachableObjectType::FIELD:
      return compare_dexfields(lhs.field, rhs.field);
    case ReachableObjectType::METHOD:
      return compare_dexmethods(lhs.method, rhs.method);

    case ReachableObjectType::ANNO:
    case ReachableObjectType::SEED: {
      // Pretty terrible, optimize.
      std::ostringstream oss1;
      oss1 << lhs;
      std::ostringstream oss2;
      oss2 << rhs;
      return oss1.str() < oss2.str();
    }
    }
    __builtin_unreachable();
  };

  bs::write_header(os, /* version */ 1);
  bs::GraphWriter<ReachableObject, ReachableObjectHash> gw(
      write_reachable_object,
      [&](const ReachableObject& obj) -> std::vector<ReachableObject> {
        if (!retainers_of.count(obj)) {
          return {};
        }
        const auto& preds = retainers_of.at(obj);
        std::vector<ReachableObject> preds_vec(preds.begin(), preds.end());
        // Gotta sort the reachables or the output is nondeterministic.
        std::sort(preds_vec.begin(), preds_vec.end(), compare);
        return preds_vec;
      });

  // Gotta sort the keys or the output is nondeterministic.
  auto key_adaptor = boost::adaptors::keys(retainers_of);
  std::vector<ReachableObject> keys(key_adaptor.begin(), key_adaptor.end());
  std::sort(keys.begin(), keys.end(), compare);
  gw.write(os, keys);
}

template void TransitiveClosureMarkerWorker::push<DexClass>(
    const DexClass* parent, const DexType* type);
} // namespace reachability
