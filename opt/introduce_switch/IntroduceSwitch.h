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

    Metrics& operator+=(const Metrics& that) {
      switch_intro += that.switch_intro;
      switch_cases += that.switch_cases;
      removed_instrs += that.removed_instrs;
      added_instrs += that.added_instrs;
      return *this;
    }
  };

  IntroduceSwitchPass() : Pass("IntroduceSwitchPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  Metrics run(DexMethod*);
};
