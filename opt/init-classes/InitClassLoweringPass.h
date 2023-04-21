/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

class InitClassLoweringPass : public Pass {
 public:
  InitClassLoweringPass() : Pass("InitClassLoweringPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoInitClassInstructions, Establishes},
        {RenameClass, Preserves},
    };
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_drop{false};
  bool m_log_init_classes{false};
  bool m_log_in_clinits{false};
};
