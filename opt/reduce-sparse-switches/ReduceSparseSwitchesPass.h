/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

class ReduceSparseSwitchesPass : public Pass {
 public:
  struct Stats {
    size_t splitting_transformations{0};
    size_t splitting_transformations_switch_cases{0};
    size_t binary_search_transformations{0};
    size_t binary_search_transformations_switch_cases{0};
    size_t field_refs_exceeded{0};
    size_t method_refs_exceeded{0};

    Stats& operator+=(const Stats&);
  };

  struct Config {
    // Starting at 10, the splitting transformation is always a code-size win
    uint64_t min_splitting_switch_cases{10};

    uint64_t min_binary_search_switch_cases_cold_per_call{400};
    uint64_t min_binary_search_switch_cases_hot_per_call{200};
    uint64_t min_binary_search_switch_cases{40};
  };

  ReduceSparseSwitchesPass() : Pass("ReduceSparseSwitchesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  std::string get_config_doc() override {
    return trim(R"(
This pass reduces sparse switch instructions.

Sparse switches are expensive at runtime when they get compiled by ART, as 
they get translated to a linear sequence of conditional branches which take
O(N) time to execute, where N is the number of switch cases.

This pass performs two transformations which are designed to improve
runtime performance:

1. Splitting sparse switches into a main packed switch and a secondary sparse
   switch. This transformation is only performed if we find a packed
   sub-sequence of case keys that contains at least half of all case keys,
   so that we shave off at least one operation from the binary search the
   interpreter needs to do over the remaining sparse case keys, making sure
   we never degrade worst-case complexity. We run this to a fixed point.
2. Replacing sparse switches with a binary search followed by a packed
   switch. This transformation is only performed if the switch is
   sufficiently large, also taking into account whether it is hot, and how
   often it(s containing method) is called. We perform the binary search over
   an array of integers. The array is cached in a static field, and lazily
   initialized when it is first needed. The initialization is done via a
   separate generated helper method, which we mark as @NeverCompile as it
   will only run once, and it only contains a handfull of instructions, one
   of which is the powerful fill-array-data instruction.
    )");
  }

  void bind_config() override;

  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static ReduceSparseSwitchesPass::Stats splitting_transformation(
      size_t min_switch_cases, DexMethod* method);

  static ReduceSparseSwitchesPass::Stats binary_search_transformation(
      size_t min_switch_cases,
      DexClass* cls,
      DexMethod* method,
      size_t& running_index,
      size_t& method_refs,
      size_t& field_refs,
      InsertOnlyConcurrentMap<DexMethod*, DexMethod*>* init_methods);

 private:
  Config m_config;
  std::optional<ReserveRefsInfoHandle> m_reserved_refs_handle;
};
