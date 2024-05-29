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
  kClassLoads,
  kClassLoadsBasicBlockFiltering,
  kExactSymbolMatch,
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

struct ModelSpec;

class InterDexGrouping final {
 public:
  explicit InterDexGrouping(const Scope& scope,
                            ConfigFiles& conf,
                            const InterDexGroupingConfig& config,
                            const ConstTypeHashSet& merging_targets)
      : m_conf(conf), m_config(config) {
    build_interdex_grouping(scope, merging_targets);
  }

  size_t num_groups() const { return m_all_interdexing_groups.size(); }

  void visit_groups(const ModelSpec& spec,
                    const TypeSet& current_group,
                    const std::function<void(const InterdexSubgroupIdx,
                                             const TypeSet&)>& visit_fn) const;

  // For testing only
  const std::vector<ConstTypeHashSet>& get_all_interdexing_groups() const {
    return m_all_interdexing_groups;
  }

  bool is_in_ordered_set(const DexType* type) const {
    if (m_config.type == InterDexGroupingType::DISABLED) {
      return false;
    }
    return m_ordered_set.count(type);
  }

 private:
  // Divide all types in the merging_targets into different interdex subgroups
  // This grouping should be applied at the entire model level.
  void build_interdex_grouping(const Scope& scope,
                               const ConstTypeHashSet& merging_targets);

  TypeSet get_types_in_group(const InterdexSubgroupIdx id,
                             const TypeSet& types) const;

  ConfigFiles& m_conf;
  const InterDexGroupingConfig m_config;
  std::vector<ConstTypeHashSet> m_all_interdexing_groups;
  // The set of types that are supposingly ordered and not in the last interdex
  // groups. It should be empty if interdex grouping is disabled.
  ConstTypeHashSet m_ordered_set;
};

std::ostream& operator<<(std::ostream& os,
                         class_merging::InterDexGroupingInferringMode mode);

}; // namespace class_merging
