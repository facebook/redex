/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "ClassHierarchy.h"

class OriginalNamePass : public Pass {
 public:
  OriginalNamePass() : Pass("OriginalNamePass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("hierarchy_roots", {}, m_hierarchy_roots);
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& cfg,
                PassManager& mgr) override;

 private:
  void build_hierarchies(
      PassManager& mgr,
      const ClassHierarchy& ch,
      Scope& scope,
      std::unordered_map<const DexType*, std::string>* hierarchies);

  std::vector<std::string> m_hierarchy_roots;
};
