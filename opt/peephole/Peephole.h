/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>
#include "Pass.h"

class PeepholePass : public Pass {
 public:
  PeepholePass() : Pass("PeepholePass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("disabled_peepholes", {}, config.disabled_peepholes);
  }

 private:
  struct Config {
    std::vector<std::string> disabled_peepholes;
  };
  Config config;
};
