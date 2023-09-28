/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class AddSecondaryDexPass : public Pass {
 public:
  AddSecondaryDexPass() : Pass("AddSecondaryDexPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoInitClassInstructions, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoUnreachableInstructions, Preserves},
        {RenameClass, Preserves},
    };
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
