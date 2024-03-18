/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "OutliningProfileGuidance.h"
#include "Pass.h"

namespace outliner {

struct Config {
  size_t min_insns_size{3};
  size_t max_insns_size{77};
  bool reorder_with_method_profiles{true};
  bool derive_method_profiles_stats{false};
  bool reuse_outlined_methods_across_dexes{true};
  size_t max_outlined_methods_per_class{100};
  size_t savings_threshold{10};
  bool outline_from_primary_dex{false};
  bool full_dbg_positions{false};
  bool debug_make_crashing{false};
  ProfileGuidanceConfig profile_guidance;
  bool outline_control_flow{true};
  bool obfuscate_method_names{false};
};

} // namespace outliner

class InstructionSequenceOutliner : public Pass {
 public:
  InstructionSequenceOutliner() : Pass("InstructionSequenceOutlinerPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, RequiresAndEstablishes},
        {NoResolvablePureRefs, Preserves},
        {SpuriousGetClassCallsInterned, RequiresAndEstablishes},
        {InitialRenameClass, Preserves},
    };
  }

  void bind_config() override;

  void run_pass(DexStoresVector& stores,
                ConfigFiles& config,
                PassManager& mgr) override;

 private:
  outliner::Config m_config;
  size_t m_iteration{0};
};
