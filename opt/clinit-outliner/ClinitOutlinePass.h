/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexUtil.h"
#include "Pass.h"

class ClinitOutlinePass : public Pass {
 public:
  ClinitOutlinePass() : Pass("ClinitOutlinePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Destroys},
        {NoResolvablePureRefs, Preserves},
        {HasSourceBlocks, Preserves},
    };
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  int64_t m_min_clinit_size{0};
  std::string m_interaction_pattern;
  int64_t m_interaction_threshold_override{0};
};
