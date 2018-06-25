/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "PassManager.h"
#include "DexClass.h"

class StaticReloPass : public Pass {
 public:
  StaticReloPass() : Pass("StaticReloPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("dont_optimize_annos", {}, m_dont_optimize_annos);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<std::string> m_dont_optimize_annos;
};
