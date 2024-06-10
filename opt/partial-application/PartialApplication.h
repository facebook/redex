/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "OutliningProfileGuidance.h"
#include "Pass.h"

struct PartialApplicationConfig {
  size_t cost_method{28};
  size_t const_signed_cost_base{1};
  size_t const_signed_cost_addon_1{1};
  size_t const_signed_cost_addon_2{1};
  size_t const_signed_cost_addon_3{2};
  size_t const_singleton_cost{2};
  size_t const_obj_or_none_cost_1{2};
  size_t const_obj_or_none_cost_2{3};
};

class PartialApplicationPass : public Pass {
 public:
  PartialApplicationPass() : Pass("PartialApplicationPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
        {SpuriousGetClassCallsInterned, RequiresAndPreserves},
    };
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_iteration{0};
  outliner::ProfileGuidanceConfig m_profile_guidance_config;
  bool m_derive_method_profiles_stats{false};
  PartialApplicationConfig m_cost_config;
};
