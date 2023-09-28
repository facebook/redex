/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {};
  }

  bool is_cfg_legacy() override { return true; }

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
