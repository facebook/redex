/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Pass.h"

class LocalMethodInlinePass : public Pass {
 public:
  LocalMethodInlinePass() : Pass("LocalMethodInlinePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
        // This may be too conservative as the inliner can be configured not to
        // DCE in the shrinker.
        {NoSpuriousGetClassCalls, RequiresAndPreserves},
    };
  }

  bool is_cfg_legacy() override { return true; }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
