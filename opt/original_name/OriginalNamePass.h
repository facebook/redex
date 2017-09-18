/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"
#include "ClassHierarchy.h"

class OriginalNamePass : public Pass {
 public:
  OriginalNamePass() : Pass("OriginalNamePass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("hierarchy_roots", {}, m_hierarchy_roots);
  }

  virtual void run_pass(DexStoresVector& stores,
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
