/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ClassHierarchy.h"
#include "DeterministicContainers.h"
#include "Pass.h"

class OriginalNamePass : public Pass {
 public:
  OriginalNamePass() : Pass("OriginalNamePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {RenameClass, Preserves},
        {InitialRenameClass, RequiresAndEstablishes},
    };
  }

  void bind_config() override {
    bind("hierarchy_roots", {}, m_hierarchy_roots);
    trait(Traits::Pass::unique, true);
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

 private:
  void build_hierarchies(
      PassManager& mgr,
      const ClassHierarchy& ch,
      Scope& scope,
      UnorderedMap<const DexType*, std::string_view>* hierarchies);

  std::vector<std::string> m_hierarchy_roots;
};
