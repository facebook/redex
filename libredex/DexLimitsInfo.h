/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexLimits.h"
#include "DexStructure.h"

class DexLimitsInfo {
  DexStructure m_dex;
  init_classes::InitClassesWithSideEffects* m_init_classes_with_side_effects;
  size_t m_linear_alloc_limit{kMaxLinearAlloc};
  size_t m_meth_limit{kMaxMethodRefs};
  size_t m_field_limit{kMaxFieldRefs};
  size_t m_type_limit{kNewMaxTypeRefs};

 public:
  explicit DexLimitsInfo(
      init_classes::InitClassesWithSideEffects* init_classes_with_side_effects)
      : m_init_classes_with_side_effects(init_classes_with_side_effects) {}
  explicit DexLimitsInfo(
      init_classes::InitClassesWithSideEffects* init_classes_with_side_effects,
      const DexClasses& dex);

  void set_method_limit(size_t limit) { m_meth_limit = limit; }
  void set_field_limits(size_t limit) { m_field_limit = limit; }
  void set_type_limits(size_t limit) { m_type_limit = limit; }

  size_t get_num_field_refs() { return m_dex.get_num_frefs(); }
  size_t get_num_method_refs() { return m_dex.get_num_mrefs(); }

  bool is_method_overflow() {
    return m_dex.get_overflow_stats().method_refs_overflow > 0;
  }
  bool is_field_overflow() {
    return m_dex.get_overflow_stats().field_refs_overflow > 0;
  }
  bool is_type_overflow() {
    return m_dex.get_overflow_stats().type_refs_overflow > 0;
  }

  // Calculate the refs after adding \p cls to current dex. If the dex is still
  // valid, update the refs and return true. Otherwise, return false.
  bool update_refs_by_adding_class(DexClass* cls);
  void update_refs_by_always_adding_class(DexClass* cls);

  // Update the the refs when cls is removed from current dex.
  void update_refs_by_erasing_class(DexClass* cls);

  const DexStructure& get_dex() const { return m_dex; }
};
