/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ClassHierarchy.h"
#include "Pass.h"

class OriginalNamePass : public Pass {
 public:
  OriginalNamePass() : Pass("OriginalNamePass") {}

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
      std::unordered_map<const DexType*, std::string_view>* hierarchies);

  std::vector<std::string> m_hierarchy_roots;
};
