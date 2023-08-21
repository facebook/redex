/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "OutliningProfileGuidance.h"
#include "Pass.h"

class PartialApplicationPass : public Pass {
 public:
  PartialApplicationPass() : Pass("PartialApplicationPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
    };
  }

  void bind_config() override;

  bool is_cfg_legacy() override { return true; }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_iteration{0};
  outliner::ProfileGuidanceConfig m_profile_guidance_config;
};
