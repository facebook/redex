/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "ShrinkerConfig.h"

class ShrinkerPass : public Pass {
 public:
  ShrinkerPass() : Pass("ShrinkerPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        // This may be too conservative as the shrinker can be configured not to
        // DCE.
        {SpuriousGetClassCallsInterned, RequiresAndPreserves},
    };
  }

  void bind_config() override;
  bool is_cfg_legacy() override { return true; }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  shrinker::ShrinkerConfig m_config;
};
