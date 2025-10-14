/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "MethodInliner.h"
#include "Pass.h"

class MethodInlinePass : public Pass {
 public:
  MethodInlinePass() : Pass("MethodInlinePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
        // This may be too conservative as the inliner can be configured not to
        // DCE in the shrinker.
        {SpuriousGetClassCallsInterned, RequiresAndPreserves},
        {NoWriteBarrierInstructions, Destroys},
    };
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  InlinerCostConfig m_inliner_cost_config;
  HotColdInliningBehavior m_hot_cold_inlining_behavior;
  bool m_partial_hot_hot;
};
