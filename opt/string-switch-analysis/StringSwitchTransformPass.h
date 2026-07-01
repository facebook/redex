/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class StringSwitchTransformPass : public Pass {
 public:
  StringSwitchTransformPass() : Pass("StringSwitchTransformPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {{DexLimitsObeyed, Preserves},
            {UltralightCodePatterns, Preserves},
            {NoInitClassInstructions, Preserves},
            {RenameClass, Preserves}};
  }

  std::string get_config_doc() override {
    return "Reports conforming instructions in methods that represent a switch "
           "over java/lang/String objects.";
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
