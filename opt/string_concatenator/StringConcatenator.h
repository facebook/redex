/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class StringConcatenatorPass : public Pass {
 public:
  StringConcatenatorPass() : Pass("StringConcatenatorPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{DexLimitsObeyed, {.preserves = true}},
            {HasSourceBlocks, {.preserves = true}},
            {NoSpuriousGetClassCalls, {.preserves = true}}};
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
