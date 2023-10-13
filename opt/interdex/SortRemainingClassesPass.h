/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class SortRemainingClassesPass : public Pass {
 public:
  explicit SortRemainingClassesPass() : Pass("SortRemainingClassesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, RequiresAndEstablishes},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void bind_config() override {
    bind("enable_pass", false, m_enable_pass,
         "Whether to enable SortRemainingClassesPass.");
    bind("sort_primary_dex", false, m_sort_primary_dex,
         "Whether to sort classes in primary dex.");
  }

  bool is_cfg_legacy() override { return true; }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  // This pass should be enabled only for apps which have betamaps and deep data
  // enabled.
  bool m_enable_pass;
  // Whether the classes in primary dex should be sorted.
  bool m_sort_primary_dex;
};
