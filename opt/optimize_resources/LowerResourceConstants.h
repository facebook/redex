/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/**
 * Undo the effect of MaterializeResourceConstantsPass.
 */
class LowerResourceConstantsPass : public Pass {
 public:
  LowerResourceConstantsPass() : Pass("LowerResourceConstantsPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
        {RenameClass, Preserves},
    };
  }

  std::string get_config_doc() override {
    return trim(R"(
A pass that replaces all instructions of the form `R_CONST vx, #I` with `CONST vx, #I` where #I is an integer
literal. This is needed as R_CONST is not a valid DEX instruction and is only used by Redex to track resources.
    )");
  }

  void bind_config() override {}
  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override {}
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
