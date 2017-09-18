/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <vector>
#include "Pass.h"

class PeepholePass : public Pass {
 public:
  PeepholePass() : Pass("PeepholePass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("disabled_peepholes", {}, config.disabled_peepholes);
  }

 private:
  struct Config {
    std::vector<std::string> disabled_peepholes;
  };
  Config config;
};
