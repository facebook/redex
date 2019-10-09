/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexStructure.h"

#include <boost/algorithm/string/predicate.hpp>
#include <vector>

#include "DexClass.h"
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
constexpr unsigned MAX_METHOD_REFS = (1 << 16) - 1;
constexpr unsigned MAX_FIELD_REFS = (1 << 16) - 1;

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
 * Removes the elements of b from a. Runs in O(size(a)), so it works best if
 * size(a) << size(b).
 */
template <typename T>
std::unordered_set<T> set_difference(const std::unordered_set<T>& a,
                                     const std::unordered_set<T>& b) {
  std::unordered_set<T> result;
  for (auto& v : a) {
    if (!b.count(v)) {
      result.emplace(v);
    }
  }
  return result;
}

} // namespace

namespace interdex {

bool DexesStructure::add_class_to_current_dex(const MethodRefs& clazz_mrefs,
                                              const FieldRefs& clazz_frefs,
                                              const TypeRefs& clazz_trefs,
                                              DexClass* clazz) {
  always_assert_log(m_classes.count(clazz) == 0,
                    "Can't emit the same class twice!\n", SHOW(clazz));

  if (m_current_dex.add_class_if_fits(
          clazz_mrefs, clazz_frefs, clazz_trefs, m_linear_alloc_limit,
          MAX_METHOD_REFS - m_reserve_mrefs, m_type_refs_limit, clazz)) {
    update_stats(clazz_mrefs, clazz_frefs, clazz);
    m_classes.emplace(clazz);
    return true;
  }

  return false;
}

void DexesStructure::add_class_no_checks(const MethodRefs& clazz_mrefs,
                                         const FieldRefs& clazz_frefs,
                                         const TypeRefs& clazz_trefs,
                                         DexClass* clazz) {
  always_assert_log(m_classes.count(clazz) == 0,
                    "Can't emit the same class twice: %s!\n", SHOW(clazz));

  auto laclazz = estimate_linear_alloc(clazz);
  m_current_dex.add_class_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs,
                                    laclazz, clazz);
  m_classes.emplace(clazz);
  update_stats(clazz_mrefs, clazz_frefs, clazz);
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

  if (dex_info.mixed_mode) {
    m_info.num_mixed_mode_dexes++;
  }

  if (dex_info.scroll) {
    m_info.num_scroll_dexes++;
  }

  m_current_dex.check_refs_count();

  DexClasses all_classes = m_current_dex.take_all_classes();

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

bool DexStructure::add_class_if_fits(const MethodRefs& clazz_mrefs,
                                     const FieldRefs& clazz_frefs,
                                     const TypeRefs& clazz_trefs,
                                     size_t linear_alloc_limit,
                                     size_t method_refs_limit,
                                     size_t type_refs_limit,
                                     DexClass* clazz) {

  unsigned laclazz = estimate_linear_alloc(clazz);
  if (m_linear_alloc_size + laclazz > linear_alloc_limit) {
    TRACE(IDEX, 6,
          "[warning]: Class won't fit current dex since it will go "
          "over the linear alloc limit: %s\n",
          SHOW(clazz));
    return false;
  }

  auto extra_mrefs = set_difference(clazz_mrefs, m_mrefs);
  auto extra_frefs = set_difference(clazz_frefs, m_frefs);
  auto extra_trefs = set_difference(clazz_trefs, m_trefs);

  if (m_mrefs.size() + extra_mrefs.size() >= method_refs_limit) {
    TRACE(IDEX, 6,
          "[warning]: Class won't fit current dex since it will go "
          "over the method refs limit: %d >= %d: %s\n",
          m_mrefs.size() + extra_mrefs.size(), method_refs_limit, SHOW(clazz));
    return false;
  }

  if (m_frefs.size() + extra_frefs.size() >= MAX_FIELD_REFS) {
    TRACE(IDEX, 6,
          "[warning]: Class won't fit current dex since it will go "
          "over the field refs limit: %d >= %d: %s\n",
          m_frefs.size() + extra_frefs.size(), MAX_FIELD_REFS, SHOW(clazz));
    return false;
  }

  if (m_trefs.size() + extra_trefs.size() >= type_refs_limit) {
    TRACE(IDEX, 6,
          "[warning]: Class won't fit current dex since it will go "
          "over the type refs limit: %d >= %d: %s\n",
          m_trefs.size() + extra_trefs.size(), type_refs_limit, SHOW(clazz));
    return false;
  }

  add_class_no_checks(clazz_mrefs, clazz_frefs, clazz_trefs, laclazz, clazz);
  return true;
}

void DexStructure::add_class_no_checks(const MethodRefs& clazz_mrefs,
                                       const FieldRefs& clazz_frefs,
                                       const TypeRefs& clazz_trefs,
                                       unsigned laclazz,
                                       DexClass* clazz) {
  TRACE(IDEX, 7, "Adding class: %s\n", SHOW(clazz));
  m_mrefs.insert(clazz_mrefs.begin(), clazz_mrefs.end());
  m_frefs.insert(clazz_frefs.begin(), clazz_frefs.end());
  m_trefs.insert(clazz_trefs.begin(), clazz_trefs.end());
  m_linear_alloc_size += laclazz;
  m_classes.push_back(clazz);
}

/*
 * Sanity check: did gather_refs return all the refs that ultimately ended up
 * in the dex?
 */
void DexStructure::check_refs_count() {
  std::vector<DexMethodRef*> mrefs;
  for (DexClass* cls : m_classes) {
    cls->gather_methods(mrefs);
  }
  std::unordered_set<DexMethodRef*> mrefs_set(mrefs.begin(), mrefs.end());
  if (mrefs_set.size() > m_mrefs.size()) {
    for (DexMethodRef* mr : mrefs_set) {
      if (!m_mrefs.count(mr)) {
        TRACE(IDEX, 4, "WARNING: Could not find %s in predicted mrefs set\n",
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
    for (auto* fr : frefs_set) {
      if (!m_frefs.count(fr)) {
        TRACE(IDEX, 4, "WARNING: Could not find %s in predicted frefs set\n",
              SHOW(fr));
      }
    }
  }

  // TODO: do we need to re-check linear_alloc_limit?
}

void DexStructure::squash_empty_last_class(DexClass* clazz) {
  always_assert(m_classes.back() == clazz);
  always_assert(clazz->get_dmethods().empty());
  always_assert(clazz->get_vmethods().empty());
  always_assert(clazz->get_sfields().empty());
  always_assert(clazz->get_ifields().empty());
  always_assert(!is_interface(clazz));
  m_classes.pop_back();
  m_trefs.erase(clazz->get_type());
  m_squashed_classes.push_back(clazz);
}

} // namespace interdex
