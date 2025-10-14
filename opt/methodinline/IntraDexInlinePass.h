/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Inliner.h"
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
  bool m_profile_guided;
  float m_profile_guided_heat_threshold;
  float m_profile_guided_heat_discount;
  float m_profile_guided_shrink_bias;
  float m_profile_guided_block_appear_threshold;

  HotColdInliningBehavior m_hot_cold_inlining_behavior;
  bool m_partial_hot_hot;
};
