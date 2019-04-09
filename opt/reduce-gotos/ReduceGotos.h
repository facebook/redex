/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ReduceGotosPass : public Pass {
 public:
  struct Stats {
    size_t removed_switches{0};
    size_t reduced_switches{0};
    size_t remaining_trivial_switches{0};
    size_t removed_sparse_switch_cases{0};
    size_t removed_packed_switch_cases{0};
    size_t replaced_gotos_with_returns{0};
    size_t removed_trailing_moves{0};
    size_t inverted_conditional_branches{0};
  };

  ReduceGotosPass() : Pass("ReduceGotosPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static Stats process_code(IRCode*);
};
