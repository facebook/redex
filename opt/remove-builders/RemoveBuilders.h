/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class RemoveBuildersPass : public Pass {
 public:
  RemoveBuildersPass() : Pass("RemoveBuildersPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("enable_buildee_constr_change", false, m_enable_buildee_constr_change);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::unordered_set<DexType*> m_builders;
  bool m_enable_buildee_constr_change;

  std::vector<DexType*> created_builders(DexMethod*);
  bool escapes_stack(DexType*, DexMethod*);
};
