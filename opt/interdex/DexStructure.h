/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <vector>

#include "DexClass.h"
#include "Pass.h"
#include "Util.h"

namespace interdex {

using MethodRefs = std::unordered_set<DexMethodRef*>;
using FieldRefs = std::unordered_set<DexFieldRef*>;
using TypeRefs = std::unordered_set<DexType*>;

struct DexInfo {
  bool primary{false};
  bool mixed_mode{false};
  bool coldstart{false};
  bool extended{false};
  bool scroll{false};
};

class DexStructure {
 public:
  DexStructure() : m_linear_alloc_size(0) {}

  size_t get_linear_alloc_size() const { return m_linear_alloc_size; }

  const DexClasses& get_all_classes() const { return m_classes; }

  const DexClasses& get_squashed_classes() const { return m_squashed_classes; }

  /**
   * Only call this if you know what you are doing. This will leave the
   * current instance is in an unusable state.
   */
  DexClasses take_all_classes() { return std::move(m_classes); }

  /**
   * Tries to add the specified class. Returns false if it doesn't fit.
   */
  bool add_class_if_fits(const MethodRefs& clazz_mrefs,
                         const FieldRefs& clazz_frefs,
                         const TypeRefs& clazz_trefs,
                         size_t linear_alloc_limit,
                         size_t method_refs_limit,
                         size_t type_refs_limit,
                         DexClass* clazz);

  void add_class_no_checks(const MethodRefs& clazz_mrefs,
                           const FieldRefs& clazz_frefs,
                           const TypeRefs& clazz_trefs,
                           unsigned laclazz,
                           DexClass* clazz);

  void check_refs_count();

  void squash_empty_last_class(DexClass* clazz);

 private:
  size_t m_linear_alloc_size;
  TypeRefs m_trefs;
  MethodRefs m_mrefs;
  FieldRefs m_frefs;
  std::vector<DexClass*> m_classes;
  std::vector<DexClass*> m_squashed_classes;
};

class DexesStructure {
 public:
  const DexClasses& get_current_dex_classes() const {
    return m_current_dex.get_all_classes();
  }

  const DexClasses& get_current_dex_squashed_classes() const {
    return m_current_dex.get_squashed_classes();
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

  void set_linear_alloc_limit(int64_t linear_alloc_limit) {
    m_linear_alloc_limit = linear_alloc_limit;
  }

  void set_type_refs_limit(int64_t type_refs_limit) {
    m_type_refs_limit = type_refs_limit;
  }

  void set_reserve_mrefs(size_t reserve_mrefs) {
    m_reserve_mrefs = reserve_mrefs;
  }

  /**
   * Tries to add the class to the current dex. If it can't, it returns false.
   * Throws if the class already exists in the dexes.
   */
  bool add_class_to_current_dex(const MethodRefs& clazz_mrefs,
                                const FieldRefs& clazz_frefs,
                                const TypeRefs& clazz_trefs,
                                DexClass* clazz);

  /*
   * Add class to current dex, without any checks.
   * Throws if the class already exists in the dexes.
   */
  void add_class_no_checks(const MethodRefs& clazz_mrefs,
                           const FieldRefs& clazz_frefs,
                           const TypeRefs& clazz_trefs,
                           DexClass* clazz);
  void add_class_no_checks(DexClass* clazz) {
    add_class_no_checks(MethodRefs(), FieldRefs(), TypeRefs(), clazz);
  }

  void squash_empty_last_class(DexClass* clazz) {
    m_current_dex.squash_empty_last_class(clazz);
  }

  /**
   * It returns the classes contained in this dex and moves on to the next dex.
   */
  DexClasses end_dex(DexInfo dex_info);

  bool has_class(DexClass* clazz) const { return m_classes.count(clazz); }

 private:
  void update_stats(const MethodRefs& clazz_mrefs,
                    const FieldRefs& clazz_frefs,
                    DexClass* clazz);

  // NOTE: Keeps track only of the last dex.
  DexStructure m_current_dex;

  // All the classes that end up added in the dexes.
  std::unordered_set<DexClass*> m_classes;

  int64_t m_linear_alloc_limit;
  int64_t m_type_refs_limit;
  size_t m_reserve_mrefs;

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

  struct DexesStats {
    size_t num_static_meths{0};
    size_t num_dmethods{0};
    size_t num_vmethods{0};
    size_t num_mrefs{0};
    size_t num_frefs{0};
  } m_stats;
};

} // namespace interdex
