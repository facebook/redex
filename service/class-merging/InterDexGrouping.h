/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConfigFiles.h"
#include "DexClass.h"

using ConstTypeHashSet = std::unordered_set<const DexType*>;
using TypeHashSet = std::unordered_set<DexType*>;
using TypeSet = std::set<const DexType*, dextypes_comparator>;

namespace class_merging {

enum InterDexGroupingType {
  DISABLED = 0, // No interdex grouping.
  NON_HOT_SET = 1, // Exclude hot set.
  NON_ORDERED_SET = 2, // Exclude all ordered set.
  FULL = 3, // Apply interdex grouping on the entire input.
};

enum class InterDexGroupingInferringMode {
  kAllTypeRefs,
  kClassLoads,
  kClassLoadsBasicBlockFiltering,
};

class InterDexGrouping final {
 public:
  explicit InterDexGrouping(ConfigFiles& conf,
                            InterDexGroupingType type,
                            InterDexGroupingInferringMode mode)
      : m_conf(conf), m_type(type), m_inferring_mode(mode) {}

  bool is_enabled() const { return m_type != InterDexGroupingType::DISABLED; }

  std::vector<ConstTypeHashSet>& group_by_interdex_set(
      const Scope& scope, const ConstTypeHashSet& types);

  TypeSet get_types_in_current_interdex_group(
      const TypeSet& types, const ConstTypeHashSet& interdex_group_types);

  static InterDexGroupingType get_merge_per_interdex_type(
      const std::string& interdex_grouping);

 private:
  ConfigFiles& m_conf;
  const InterDexGroupingType m_type;
  const InterDexGroupingInferringMode m_inferring_mode;
  std::vector<ConstTypeHashSet> m_all_interdexing_groups;
};

std::ostream& operator<<(std::ostream& os,
                         class_merging::InterDexGroupingInferringMode mode);

}; // namespace class_merging
