/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class UpCodeMotionPass : public Pass {
 public:
  struct Stats {
    size_t instructions_moved{0};
    size_t branches_moved_over{0};
    size_t inverted_conditional_branches{0};
  };

  UpCodeMotionPass() : Pass("UpCodeMotionPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static Stats process_code(IRCode*);
};
