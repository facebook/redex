/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class IntroduceSwitchPass : public Pass {
 public:
  struct Metrics {
    int32_t switch_intro{0};
    int32_t switch_cases{0};
    int32_t removed_instrs{0};
    int32_t added_instrs{0};

    Metrics() {}

    Metrics operator+(const Metrics& b) {
      Metrics m;
      m.switch_intro = this->switch_intro + b.switch_intro;
      m.switch_cases = this->switch_cases + b.switch_cases;
      m.removed_instrs = this->removed_instrs + b.removed_instrs;
      m.added_instrs = this->added_instrs + b.added_instrs;
      return m;
    }
  };

  IntroduceSwitchPass() : Pass("IntroduceSwitchPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  Metrics run(DexMethod*);
};
