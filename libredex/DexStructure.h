/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <vector>

#include "DexClass.h"
#include "InitClassesWithSideEffects.h"
#include "Pass.h"
#include "Util.h"

unsigned estimate_linear_alloc(const DexClass* clazz);

struct ReserveRefsInfo {
  size_t frefs;
  size_t trefs;
  size_t mrefs;

  ReserveRefsInfo()
      : ReserveRefsInfo(/* frefs */ 0, /* trefs */ 0, /* mrefs */ 0) {}

  ReserveRefsInfo(size_t frefs, size_t trefs, size_t mrefs)
      : frefs(frefs), trefs(trefs), mrefs(mrefs) {}
};

using MethodRefs = std::unordered_set<DexMethodRef*>;
using FieldRefs = std::unordered_set<DexFieldRef*>;
using TypeRefs = std::unordered_set<DexType*>;

struct DexInfo {
  bool primary{false};
  bool coldstart{false};
  bool background{false};
  bool extended{false};
  bool scroll{false};
  bool betamap_ordered{false};
};

struct OverflowStats {
  size_t linear_alloc_overflow{0};
  size_t method_refs_overflow{0};
  size_t field_refs_overflow{0};
  size_t type_refs_overflow{0};

  OverflowStats& operator+=(const OverflowStats& rhs) {
    linear_alloc_overflow += rhs.linear_alloc_overflow;
    method_refs_overflow += rhs.method_refs_overflow;
    field_refs_overflow += rhs.field_refs_overflow;
    type_refs_overflow += rhs.type_refs_overflow;
    return *this;
  }
};

class DexStructure {
 public:
  DexStructure() : m_linear_alloc_size(0) {}

  bool empty() const { return m_classes_iterators.empty(); }

  DexClasses get_classes() const {
    return DexClasses(m_classes.begin(), m_classes.end());
  }

  /**
   * Tries to add the specified class. Returns false if it doesn't fit.
   */
  bool add_class_if_fits(const MethodRefs& clazz_mrefs,
                         const FieldRefs& clazz_frefs,
                         const TypeRefs& clazz_trefs,
                         const TypeRefs& pending_init_class_fields,
                         const TypeRefs& pending_init_class_types,
                         size_t linear_alloc_limit,
                         size_t field_refs_limit,
                         size_t method_refs_limit,
                         size_t type_refs_limit,
                         DexClass* clazz);

  void add_class_no_checks(const MethodRefs& clazz_mrefs,
                           const FieldRefs& clazz_frefs,
                           const TypeRefs& clazz_trefs,
                           const TypeRefs& pending_init_class_fields,
                           const TypeRefs& pending_init_class_types,
                           unsigned laclazz,
                           DexClass* clazz);

  void add_refs_no_checks(const MethodRefs& clazz_mrefs,
                          const FieldRefs& clazz_frefs,
                          const TypeRefs& clazz_trefs,
                          const TypeRefs& pending_init_class_fields,
                          const TypeRefs& pending_init_class_types);

  /* Remove \p clazz from current dex, and update the refs.
This implementation is conservative, in that it leave behind the counters in a
way that would allow detecting any later illegal addition of classes, but may
also reject some legal cases.
   */
  void remove_class(const init_classes::InitClassesWithSideEffects*
                        init_classes_with_side_effects,
                    const MethodRefs& clazz_mrefs,
                    const FieldRefs& clazz_frefs,
                    const TypeRefs& clazz_trefs,
                    const TypeRefs& pending_init_class_fields,
                    const TypeRefs& pending_init_class_types,
                    unsigned laclazz,
                    DexClass* clazz);

  void resolve_init_classes(const init_classes::InitClassesWithSideEffects*
                                init_classes_with_side_effects,
                            const FieldRefs& frefs,
                            const TypeRefs& trefs,
                            const TypeRefs& itrefs,
                            TypeRefs* pending_init_class_fields,
                            TypeRefs* pending_init_class_types);

  bool has_tref(DexType* type) const { return m_trefs.count(type); }

  void check_refs_count();

  size_t size() const { return m_classes_iterators.size(); }

  size_t get_tref_occurrences(DexType* type) const {
    auto it = m_trefs.find(type);
    return it == m_trefs.end() ? 0 : it->second;
  }

  size_t get_mref_occurrences(DexMethodRef* method) const {
    auto it = m_mrefs.find(method);
    return it == m_mrefs.end() ? 0 : it->second;
  }

  size_t get_fref_occurrences(DexFieldRef* field) const {
    auto it = m_frefs.find(field);
    return it == m_frefs.end() ? 0 : it->second;
  }

  size_t get_num_classes() const { return m_classes.size(); }

  size_t get_num_mrefs() const { return m_mrefs.size(); }

  size_t get_num_frefs() const { return m_frefs.size(); }

  const OverflowStats& get_overflow_stats() const { return m_overflow_stats; }

 private:
  size_t m_linear_alloc_size;
  std::unordered_map<DexType*, size_t> m_trefs;
  std::unordered_map<DexMethodRef*, size_t> m_mrefs;
  std::unordered_map<DexFieldRef*, size_t> m_frefs;
  TypeRefs m_pending_init_class_fields;
  TypeRefs m_pending_init_class_types;
  std::list<DexClass*> m_classes;
  std::unordered_map<DexClass*, std::list<DexClass*>::iterator>
      m_classes_iterators;

  OverflowStats m_overflow_stats{};

  friend class DexesStructure;
};

class DexesStructure {
 public:
  const DexStructure& get_current_dex() const { return m_current_dex; }

  bool current_dex_has_tref(DexType* type) const {
    return m_current_dex.has_tref(type);
  }

  size_t get_num_coldstart_dexes() const { return m_info.num_coldstart_dexes; }

  size_t get_num_extended_dexes() const {
    return m_info.num_extended_set_dexes;
  }

  size_t get_num_scroll_dexes() const { return m_info.num_scroll_dexes; }

  size_t get_num_dexes() const { return m_info.num_dexes; }

  size_t get_num_mixedmode_dexes() const { return m_info.num_mixed_mode_dexes; }

  size_t get_num_secondary_dexes() const { return m_info.num_secondary_dexes; }

  size_t get_num_classes() const { return m_classes.size(); }

  size_t get_num_mrefs() const { return m_stats.num_mrefs; }

  size_t get_num_frefs() const { return m_stats.num_frefs; }

  size_t get_num_dmethods() const { return m_stats.num_dmethods; }

  size_t get_num_vmethods() const { return m_stats.num_vmethods; }

  size_t get_frefs_limit() const;
  size_t get_trefs_limit() const;
  size_t get_mrefs_limit() const;

  void set_linear_alloc_limit(int64_t linear_alloc_limit) {
    m_linear_alloc_limit = linear_alloc_limit;
  }

  void set_reserve_frefs(size_t reserve_frefs) {
    m_reserve_refs.frefs = reserve_frefs;
  }

  void set_reserve_trefs(size_t reserve_trefs) {
    m_reserve_refs.trefs = reserve_trefs;
  }

  void set_reserve_mrefs(size_t reserve_mrefs) {
    m_reserve_refs.mrefs = reserve_mrefs;
  }

  void set_min_sdk(int min_sdk) { m_min_sdk = min_sdk; }

  void set_init_classes_with_side_effects(
      const init_classes::InitClassesWithSideEffects*
          init_classes_with_side_effects) {
    m_init_classes_with_side_effects = init_classes_with_side_effects;
  }

  /**
   * Tries to add the class to the current dex. If it can't, it returns false.
   * Throws if the class already exists in the dexes.
   */
  bool add_class_to_current_dex(const MethodRefs& clazz_mrefs,
                                const FieldRefs& clazz_frefs,
                                const TypeRefs& clazz_trefs,
                                const TypeRefs& clazz_itrefs,
                                DexClass* clazz);

  /*
   * Add class to current dex, without any checks.
   * Throws if the class already exists in the dexes.
   */
  void add_class_no_checks(const MethodRefs& clazz_mrefs,
                           const FieldRefs& clazz_frefs,
                           const TypeRefs& clazz_trefs,
                           const TypeRefs& clazz_itrefs,
                           DexClass* clazz);
  void add_class_no_checks(DexClass* clazz) {
    add_class_no_checks(MethodRefs(), FieldRefs(), TypeRefs(), TypeRefs(),
                        clazz);
  }
  void add_refs_no_checks(const MethodRefs& clazz_mrefs,
                          const FieldRefs& clazz_frefs,
                          const TypeRefs& clazz_trefs,
                          const TypeRefs& clazz_itrefs);

  void resolve_init_classes(const FieldRefs& frefs,
                            const TypeRefs& trefs,
                            const TypeRefs& itrefs,
                            TypeRefs* pending_init_class_fields,
                            TypeRefs* pending_init_class_types);

  /**
   * It returns the classes contained in this dex and moves on to the next dex.
   */
  DexClasses end_dex(DexInfo dex_info);

  bool has_class(DexClass* clazz) const { return m_classes.count(clazz); }

  const std::vector<DexInfo>& get_dex_info() const { return m_dex_info; }

  const OverflowStats& get_overflow_stats() const { return m_overflow_stats; }

 private:
  void update_stats(const MethodRefs& clazz_mrefs,
                    const FieldRefs& clazz_frefs,
                    DexClass* clazz);

  // NOTE: Keeps track only of the last dex.
  DexStructure m_current_dex;

  // All the classes that end up added in the dexes.
  std::unordered_set<DexClass*> m_classes;

  int64_t m_linear_alloc_limit;
  ReserveRefsInfo m_reserve_refs;
  int m_min_sdk;
  const init_classes::InitClassesWithSideEffects*
      m_init_classes_with_side_effects{nullptr};
  struct DexesInfo {
    size_t num_dexes{0};

    // Number of secondary dexes emitted.
    size_t num_secondary_dexes{0};

    // Number of coldstart dexes emitted.
    size_t num_coldstart_dexes{0};

    // Number of coldstart extended set dexes emitted.
    size_t num_extended_set_dexes{0};

    // Number of dexes containing scroll classes.
    size_t num_scroll_dexes{0};

    // Number of mixed mode dexes;
    size_t num_mixed_mode_dexes{0};
  } m_info;
  std::vector<DexInfo> m_dex_info;

  struct DexesStats {
    size_t num_static_meths{0};
    size_t num_dmethods{0};
    size_t num_vmethods{0};
    size_t num_mrefs{0};
    size_t num_frefs{0};
  } m_stats;

  OverflowStats m_overflow_stats{};
};
