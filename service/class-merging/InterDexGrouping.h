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

struct InterDexGroupingConfig {
  explicit InterDexGroupingConfig(InterDexGroupingType type)
      : type(type),
        inferring_mode(InterDexGroupingInferringMode::kClassLoads) {}

  InterDexGroupingType type;
  InterDexGroupingInferringMode inferring_mode;

  bool is_enabled() const { return type != DISABLED; }

  void init_type(const std::string& interdex_grouping);
  void init_inferring_mode(const std::string& mode);
};

class InterDexGrouping final {
 public:
  explicit InterDexGrouping(ConfigFiles& conf, InterDexGroupingConfig config)
      : m_conf(conf), m_config(config) {}

  std::vector<ConstTypeHashSet>& group_by_interdex_set(
      const Scope& scope, const ConstTypeHashSet& types);

  TypeSet get_types_in_current_interdex_group(
      const TypeSet& types, const ConstTypeHashSet& interdex_group_types);

 private:
  ConfigFiles& m_conf;
  const InterDexGroupingConfig m_config;
  std::vector<ConstTypeHashSet> m_all_interdexing_groups;
};

std::ostream& operator<<(std::ostream& os,
                         class_merging::InterDexGroupingInferringMode mode);

}; // namespace class_merging
