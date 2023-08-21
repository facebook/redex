/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "InterDexPass.h"
#include "Pass.h"
#include "PluginRegistry.h"

class TransformConstClassBranchesPass : public Pass {
 public:
  TransformConstClassBranchesPass() : Pass("TransformConstClassBranchesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
        {RenameClass, Preserves},
    };
  }
  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_consider_external_classes;
  size_t m_min_cases;
  size_t m_max_cases;
  std::string m_string_tree_integrity_method;
  std::string m_string_tree_lookup_method;
};
