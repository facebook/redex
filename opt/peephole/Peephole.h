/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include <vector>

class PeepholePass : public Pass {
 public:
  PeepholePass() : Pass("PeepholePass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void bind_config() override {
    bind("disabled_peepholes", {}, config.disabled_peepholes);
  }

 private:
  struct Config {
    std::vector<std::string> disabled_peepholes;
  };
  Config config;
};
