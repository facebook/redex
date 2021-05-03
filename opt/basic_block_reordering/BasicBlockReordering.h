/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class BasicBlockReorderingPass : public Pass {
 public:
  struct Config {
    size_t interaction_profile;
    float low_appearance_threshold;
  };
  BasicBlockReorderingPass() : Pass("BasicBlockReorderingPass") {}
  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;
  void bind_config() override {
    bind(
        "interaction_profile", 0 /* ColdStart */, m_config.interaction_profile);
    bind("low_appearance_threshold", 0.0f, m_config.low_appearance_threshold);
  }

 private:
  Config m_config;
};
