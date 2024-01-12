/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexStructure.h"

#include <boost/algorithm/string/predicate.hpp>
#include <vector>

#include "DexClass.h"
#include "DexLimits.h"
#include "Show.h"
#include "Trace.h"
#include "Util.h"

namespace {
struct PenaltyPattern {
  const char* suffix;
  unsigned penalty;

  PenaltyPattern(const char* str, unsigned penalty)
      : suffix(str), penalty(penalty) {}
};

const PenaltyPattern PENALTY_PATTERNS[] = {
    {"Layout;", 1500},
    {"View;", 1500},
    {"ViewGroup;", 1800},
    {"Activity;", 1500},
};

constexpr unsigned VTABLE_SLOT_SIZE = 4;
constexpr unsigned OBJECT_VTABLE = 48;
constexpr unsigned METHOD_SIZE = 52;
constexpr unsigned INSTANCE_FIELD_SIZE = 16;
constexpr size_t MAX_METHOD_REFS = kMaxMethodRefs - 1;
constexpr size_t MAX_FIELD_REFS = kMaxFieldRefs - 1;
inline size_t MAX_TYPE_REFS(int min_sdk) {
  return get_max_type_refs(min_sdk) - 1;
}

bool matches_penalty(const char* str, unsigned* penalty) {
  for (auto const& pattern : PENALTY_PATTERNS) {
    if (boost::algorithm::ends_with(str, pattern.suffix)) {
      *penalty = pattern.penalty;
      return true;
    }
  }
  return false;
}

/**
 * Returns the count of elements present in a but not in b.
 */
template <typename T>
size_t set_difference_size(const std::unordered_set<T>& a,
                           const std::unordered_set<T>& b) {
  size_t result = a.size();
  if (a.size() <= b.size()) {
    for (const auto& v : a) {
      result -= b.count(v);
    }
  } else {
    for (const auto& v : b) {
      result -= a.count(v);
    }
  }
  return result;
}

} // namespace

/**
 * Estimates the linear alloc space consumed by the class at runtime.
 */
unsigned estimate_linear_alloc(const DexClass* clazz) {
  unsigned lasize = 0;
  // VTable guesstimate. Technically we could do better here, but only so much.
  // Try to stay bug-compatible with DalvikStatsTool.
  if (!is_interface(clazz)) {
    unsigned vtablePenalty = OBJECT_VTABLE;
    if (!matches_penalty(clazz->get_type()->get_name()->c_str(),
                         &vtablePenalty) &&
        clazz->get_super_class() != nullptr) {

      // TODO: we could be redexing object some day... :)
      matches_penalty(clazz->get_super_class()->get_name()->c_str(),
                      &vtablePenalty);
    }
    lasize += vtablePenalty;
    lasize += clazz->get_vmethods().size() * VTABLE_SLOT_SIZE;
  }

  lasize += clazz->get_dmethods().size() * METHOD_SIZE;
  lasize += clazz->get_vmethods().size() * METHOD_SIZE;
  lasize += clazz->get_ifields().size() * INSTANCE_FIELD_SIZE;

  return lasize;
}

/**
 * Returns the count of elements present in a but not in b.
 */
template <typename T>
size_t set_difference_size(const std::unordered_set<T>& a,
                           const std::unordered_map<T, size_t>& b) {
  size_t result = a.size();
  if (a.size() <= b.size()) {
    for (const auto& v : a) {
      result -= b.count(v);
    }
  } else {
    for (const auto [v, c] : b) {
      result -= a.count(v);
    }
  }
  return result;
}

size_t DexesStructure::get_frefs_limit() const {
  return MAX_FIELD_REFS - m_reserve_refs.frefs;
}
size_t DexesStructure::get_trefs_limit() const {
  return MAX_TYPE_REFS(m_min_sdk) - m_reserve_refs.trefs;
}
size_t DexesStructure::get_mrefs_limit() const {
  return MAX_METHOD_REFS - m_reserve_refs.mrefs;
}

void DexesStructure::resolve_init_classes(const FieldRefs& frefs,
                                          const TypeRefs& trefs,
                                          const TypeRefs& itrefs,
                                          TypeRefs* pending_init_class_fields,
                                          TypeRefs* pending_init_class_types) {
  m_current_dex.resolve_init_classes(m_init_classes_with_side_effects, frefs,
                                     trefs, itrefs, pending_init_class_fields,
                                     pending_init_class_types);
}

bool DexesStructure::add_class_to_current_dex(const MethodRefs& clazz_mrefs,
                                              const FieldRefs& clazz_frefs,
                                              const TypeRefs& clazz_trefs,
                                              const TypeRefs& clazz_itrefs,
                                              DexClass* clazz) {
  always_assert_log(m_classes.count(clazz) == 0,
                    "Can't emit the same class twice! %s", SHOW(clazz));

  TypeRefs pending_init_class_fields;
  TypeRefs pending_init_class_types;
  resolve_init_classes(clazz_frefs, clazz_trefs, clazz_itrefs,
                       &pending_init_class_fields, &pending_init_class_types);
  if (m_current_dex.add_class_if_fits(
          clazz_mrefs, clazz_frefs, clazz_trefs, pending_init_class_fields,
          pending_init_class_types, m_linear_alloc_limit, get_frefs_limit(),
          get_mrefs_limit(), get_trefs_limit(), clazz)) {
    update_stats(clazz_mrefs, clazz_frefs, clazz);
    m_classes.emplace(clazz);
    return true;
  }

  return false;
}

void DexesStructure::add_class_no_checks(const MethodRefs& clazz_mrefs,
                                         const FieldRefs& clazz_frefs,
                                         const TypeRefs& clazz_trefs,
                                         const TypeRefs& clazz_itrefs,
                                         DexClass* clazz) {
  always_assert_log(m_classes.count(clazz) == 0,
                    "Can't emit the same class twice: %s!\n", SHOW(clazz));

  TypeRefs pending_init_class_fields;
  TypeRefs pending_init_class_types;
  resolve_init_classes(clazz_frefs, clazz_trefs, clazz_itrefs,
                       &pending_init_class_fields, &pending_init_class_types);
  auto laclazz = estimate_linear_alloc(clazz);
  m_current_dex.add_class_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs,
                                    pending_init_class_fields,
                                    pending_init_class_types, laclazz, clazz);
  m_classes.emplace(clazz);
  update_stats(clazz_mrefs, clazz_frefs, clazz);
}

void DexesStructure::add_refs_no_checks(const MethodRefs& clazz_mrefs,
                                        const FieldRefs& clazz_frefs,
                                        const TypeRefs& clazz_trefs,
                                        const TypeRefs& clazz_itrefs) {
  TypeRefs pending_init_class_fields;
  TypeRefs pending_init_class_types;
  resolve_init_classes(clazz_frefs, clazz_trefs, clazz_itrefs,
                       &pending_init_class_fields, &pending_init_class_types);
  m_current_dex.add_refs_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs,
                                   pending_init_class_fields,
                                   pending_init_class_types);
}

DexClasses DexesStructure::end_dex(DexInfo dex_info) {
  m_info.num_dexes++;

  if (!dex_info.primary) {
    m_info.num_secondary_dexes++;
  }

  if (dex_info.coldstart) {
    m_info.num_coldstart_dexes++;
  }

  if (dex_info.extended) {
    m_info.num_extended_set_dexes++;
  }

  if (dex_info.scroll) {
    m_info.num_scroll_dexes++;
  }

  m_dex_info.push_back(dex_info);

  m_current_dex.check_refs_count();

  DexClasses all_classes = m_current_dex.get_classes();

  m_overflow_stats += m_current_dex.m_overflow_stats;

  m_current_dex = DexStructure();
  return all_classes;
}

void DexesStructure::update_stats(const MethodRefs& clazz_mrefs,
                                  const FieldRefs& clazz_frefs,
                                  DexClass* clazz) {
  for (DexMethod* method : clazz->get_dmethods()) {
    if (is_static(method)) {
      m_stats.num_static_meths++;
    }
  }
  m_stats.num_dmethods += clazz->get_dmethods().size();
  m_stats.num_vmethods += clazz->get_vmethods().size();
  m_stats.num_mrefs += clazz_mrefs.size();
  m_stats.num_frefs += clazz_frefs.size();
}

void DexStructure::resolve_init_classes(
    const init_classes::InitClassesWithSideEffects*
        init_classes_with_side_effects,
    const FieldRefs& frefs,
    const TypeRefs& trefs,
    const TypeRefs& itrefs,
    TypeRefs* pending_init_class_fields,
    TypeRefs* pending_init_class_types) {
  if (!init_classes_with_side_effects || itrefs.empty()) {
    return;
  }
  std::unordered_set<DexType*> refined_types;
  for (auto type : itrefs) {
    auto refined_type = init_classes_with_side_effects->refine(type);
    if (refined_type) {
      refined_types.insert(const_cast<DexType*>(refined_type));
    }
  }
  for (auto type : refined_types) {
    auto cls = type_class(type);
    always_assert(cls);
    if (m_pending_init_class_fields.count(type)) {
      continue;
    }
    const auto& fields = cls->get_sfields();
    if (std::any_of(fields.begin(), fields.end(), [&](DexField* field) {
          return m_frefs.count(field) || frefs.count(field);
        })) {
      continue;
    }
    pending_init_class_fields->insert(type);
    always_assert(!m_pending_init_class_types.count(type));
    if (!m_trefs.count(type) && !trefs.count(type)) {
      pending_init_class_types->insert(type);
    }
  }
}

bool DexStructure::add_class_if_fits(const MethodRefs& clazz_mrefs,
                                     const FieldRefs& clazz_frefs,
                                     const TypeRefs& clazz_trefs,
                                     const TypeRefs& pending_init_class_fields,
                                     const TypeRefs& pending_init_class_types,
                                     size_t linear_alloc_limit,
                                     size_t field_refs_limit,
                                     size_t method_refs_limit,
                                     size_t type_refs_limit,
                                     DexClass* clazz) {

  auto trace_details = [&]() {
    TRACE(IDEX, 7,
          "Current dex has %zu linear-alloc-size, %zu mrefs, %zu frefs + %zu "
          "pending-init-class-fields, %zu trefs + %zu pending-init-class-types",
          m_linear_alloc_size, m_mrefs.size(), m_frefs.size(),
          m_pending_init_class_fields.size(), m_trefs.size(),
          m_pending_init_class_types.size());
  };

  unsigned laclazz = estimate_linear_alloc(clazz);
  if (m_linear_alloc_size + laclazz > linear_alloc_limit) {
    TRACE(IDEX, 6,
          "[warning]: Class won't fit current dex since it will go "
          "over the linear alloc limit: %s",
          SHOW(clazz));
    trace_details();
    ++m_overflow_stats.linear_alloc_overflow;
    return false;
  }

  const auto extra_mrefs_size = set_difference_size(clazz_mrefs, m_mrefs);
  const auto new_method_refs = m_mrefs.size() + extra_mrefs_size;
  if (new_method_refs >= method_refs_limit) {
    TRACE(IDEX, 6,
          "[warning]: Class won't fit current dex since it will go "
          "over the method refs limit: %zu >= %zu: %s",
          m_mrefs.size() + extra_mrefs_size, method_refs_limit, SHOW(clazz));
    trace_details();
    ++m_overflow_stats.method_refs_overflow;
    return false;
  }

  const auto extra_frefs_size = set_difference_size(clazz_frefs, m_frefs);
  const auto new_field_refs = m_frefs.size() + extra_frefs_size +
                              m_pending_init_class_fields.size() +
                              pending_init_class_fields.size();
  if (new_field_refs >= field_refs_limit) {
    TRACE(IDEX, 6,
          "[warning]: Class won't fit current dex since it will go "
          "over the field refs limit: %zu >= %zu: %s",
          new_field_refs, field_refs_limit, SHOW(clazz));
    trace_details();
    ++m_overflow_stats.field_refs_overflow;
    return false;
  }

  const auto extra_trefs_size = set_difference_size(clazz_trefs, m_trefs);
  const auto new_type_refs = m_trefs.size() + extra_trefs_size +
                             m_pending_init_class_types.size() +
                             pending_init_class_types.size();
  if (new_type_refs >= type_refs_limit) {
    TRACE(IDEX, 6,
          "[warning]: Class won't fit current dex since it will go "
          "over the type refs limit: %zu >= %zu: %s",
          new_type_refs, type_refs_limit, SHOW(clazz));
    trace_details();
    ++m_overflow_stats.type_refs_overflow;
    return false;
  }

  add_class_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs,
                      pending_init_class_fields, pending_init_class_types,
                      laclazz, clazz);
  return true;
}

void DexStructure::add_class_no_checks(
    const MethodRefs& clazz_mrefs,
    const FieldRefs& clazz_frefs,
    const TypeRefs& clazz_trefs,
    const TypeRefs& pending_init_class_fields,
    const TypeRefs& pending_init_class_types,
    unsigned laclazz,
    DexClass* clazz) {
  add_refs_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs,
                     pending_init_class_fields, pending_init_class_types);
  m_linear_alloc_size += laclazz;
  m_classes.push_back(clazz);
  auto emplaced =
      m_classes_iterators.emplace(clazz, std::prev(m_classes.end())).second;
  always_assert(emplaced);
}

void DexStructure::add_refs_no_checks(
    const MethodRefs& clazz_mrefs,
    const FieldRefs& clazz_frefs,
    const TypeRefs& clazz_trefs,
    const TypeRefs& pending_init_class_fields,
    const TypeRefs& pending_init_class_types) {
  for (auto mref : clazz_mrefs) {
    m_mrefs[mref]++;
  }
  for (auto fref : clazz_frefs) {
    if (++m_frefs[fref] > 1) {
      continue;
    }
    if (!fref->is_def()) {
      continue;
    }
    auto it = m_pending_init_class_fields.find(fref->get_class());
    if (it == m_pending_init_class_fields.end()) {
      continue;
    }
    auto f = fref->as_def();
    if (is_static(f)) {
      m_pending_init_class_fields.erase(it);
    }
  }
  for (auto type : clazz_trefs) {
    if (++m_trefs[type] > 1) {
      continue;
    }
    m_pending_init_class_types.erase(type);
  }
  for (auto type : pending_init_class_fields) {
    auto inserted = m_pending_init_class_fields.insert(type).second;
    always_assert(inserted);
  }
  for (auto type : pending_init_class_types) {
    auto inserted = m_pending_init_class_types.insert(type).second;
    always_assert(inserted);
    always_assert(!m_trefs.count(type));
  }
}

void DexStructure::remove_class(const init_classes::InitClassesWithSideEffects*
                                    init_classes_with_side_effects,
                                const MethodRefs& clazz_mrefs,
                                const FieldRefs& clazz_frefs,
                                const TypeRefs& clazz_trefs,
                                const TypeRefs& pending_init_class_fields,
                                const TypeRefs& pending_init_class_types,
                                unsigned laclazz,
                                DexClass* clazz) {
  for (auto mref : clazz_mrefs) {
    auto it = m_mrefs.find(mref);
    if (--it->second == 0) {
      m_mrefs.erase(it);
    }
  }
  for (auto fref : clazz_frefs) {
    auto it = m_frefs.find(fref);
    if (--it->second > 0) {
      continue;
    }
    m_frefs.erase(it);
    if (!fref->is_def()) {
      continue;
    }
    auto f = fref->as_def();
    if (!is_static(f)) {
      continue;
    }
    auto type = fref->get_class();
    auto cls = type_class(type);
    if (cls->is_external()) {
      continue;
    }
    const auto& fields = cls->get_sfields();
    if (std::any_of(fields.begin(), fields.end(),
                    [&](DexField* field) { return m_frefs.count(field); })) {
      continue;
    }
    if (init_classes_with_side_effects->refine(type) != fref->get_class()) {
      continue;
    }
    auto inserted = m_pending_init_class_fields.insert(type).second;
    always_assert(inserted);
    if (!m_trefs.count(type) && !clazz_trefs.count(type)) {
      m_pending_init_class_types.insert(fref->get_class());
    }
  }
  for (auto type : clazz_trefs) {
    auto it = m_trefs.find(type);
    if (--it->second > 0) {
      continue;
    }
    m_trefs.erase(it);
    if (!m_pending_init_class_fields.count(type)) {
      continue;
    }
    auto inserted = m_pending_init_class_types.insert(type).second;
    always_assert(inserted);
  }
  m_linear_alloc_size -= laclazz;
  auto classes_iterators_it = m_classes_iterators.find(clazz);
  always_assert(classes_iterators_it != m_classes_iterators.end());
  auto classes_it = classes_iterators_it->second;
  m_classes.erase(classes_it);
  m_classes_iterators.erase(classes_iterators_it);
}

/*
 * Sanity check: did gather_refs return all the refs that ultimately ended up
 * in the dex?
 */
void DexStructure::check_refs_count() {
  if (!traceEnabled(IDEX, 4)) {
    return;
  }

  std::vector<DexMethodRef*> mrefs;
  for (DexClass* cls : m_classes) {
    cls->gather_methods(mrefs);
  }
  std::unordered_set<DexMethodRef*> mrefs_set(mrefs.begin(), mrefs.end());
  if (mrefs_set.size() > m_mrefs.size()) {
    std::vector<DexMethodRef*> mrefs_vec(mrefs_set.begin(), mrefs_set.end());
    std::sort(mrefs_vec.begin(), mrefs_vec.end(), compare_dexmethods);
    for (DexMethodRef* mr : mrefs_vec) {
      if (!m_mrefs.count(mr)) {
        TRACE(IDEX, 4, "WARNING: Could not find %s in predicted mrefs set",
              SHOW(mr));
      }
    }
  }

  std::vector<DexFieldRef*> frefs;
  for (DexClass* cls : m_classes) {
    cls->gather_fields(frefs);
  }
  std::unordered_set<DexFieldRef*> frefs_set(frefs.begin(), frefs.end());
  if (frefs_set.size() > m_frefs.size()) {
    std::vector<DexFieldRef*> frefs_vec(frefs_set.begin(), frefs_set.end());
    std::sort(frefs_vec.begin(), frefs_vec.end(), compare_dexfields);
    for (auto* fr : frefs_vec) {
      if (!m_frefs.count(fr)) {
        TRACE(IDEX, 4, "WARNING: Could not find %s in predicted frefs set",
              SHOW(fr));
      }
    }
  }

  // TODO: do we need to re-check linear_alloc_limit?
}
