/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Pass.h"

class DexMethod;
class IRCode;

namespace method_profiles {
class MethodProfiles;
} // namespace method_profiles

class SplitHugeSwitchPass : public Pass {
 public:
  struct Stats {
    // For debugging purposes.
    std::unordered_set<const DexMethod*> large_methods_set;
    std::unordered_set<const DexMethod*> switch_methods_set;
    std::unordered_set<const DexMethod*> large_switches_set;
    std::unordered_set<const DexMethod*> easy_expr_set;

    // Source methods, with size before and cumulative size after.
    std::unordered_map<DexMethod*, std::pair<size_t, size_t>> transformed_srcs;
    // Actual new methods inserted into their respective classes.
    std::unordered_set<DexMethod*> new_methods;

    uint32_t constructor = 0;
    uint32_t non_simple_chain = 0;
    uint32_t split_sources = 0;
    uint32_t not_hot = 0;
    uint32_t no_slots = 0;

    Stats& operator+=(const Stats& rhs);
  };

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
    };
  }

  SplitHugeSwitchPass() : Pass("SplitHugeSwitchPass") {}

  void bind_config() override;
  bool is_cfg_legacy() override { return true; }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static Stats run(DexMethod* m,
                   IRCode* code,
                   size_t code_units_threshold,
                   size_t case_threshold,
                   const method_profiles::MethodProfiles& method_profiles,
                   double hotness_threshold);

 private:
  std::string m_method_filter;
  bool m_consider_methods_too_large_for_inlining = false;
  float m_hotness_threshold = 0.0;
  uint32_t m_method_size = 0;
  uint32_t m_method_size_when_too_large_for_inlining = 0;
  uint32_t m_switch_size = 0;
  bool m_debug = false;
};
