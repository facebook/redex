/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Pass.h"

class IntraDexInlinePass : public Pass {
 public:
  IntraDexInlinePass() : Pass("IntraDexInlinePass") {}

  void bind_config() override;

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        // This may be too conservative as the inliner can be configured not
        // to DCE in the shrinker.
        {SpuriousGetClassCallsInterned, RequiresAndPreserves},
        {InitialRenameClass, Preserves},
        {NoWriteBarrierInstructions, Destroys},
    };
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_baseline_profile_guided;
  float m_baseline_profile_heat_threshold;
  float m_baseline_profile_heat_discount;
  float m_baseline_profile_shrink_bias;

  bool m_consider_hot_cold;
  bool m_partial_hot_hot;
};
