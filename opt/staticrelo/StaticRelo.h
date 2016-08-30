/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
