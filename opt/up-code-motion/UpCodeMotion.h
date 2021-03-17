/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "Pass.h"
#include "Trace.h"

class UpCodeMotionPass : public Pass {
 public:
  struct Stats {
    size_t instructions_moved{0};
    size_t branches_moved_over{0};
    size_t inverted_conditional_branches{0};
    size_t clobbered_registers{0};

    Stats& operator+=(const Stats& that) {
      instructions_moved += that.instructions_moved;
      branches_moved_over += that.branches_moved_over;
      inverted_conditional_branches += that.inverted_conditional_branches;
      clobbered_registers += that.clobbered_registers;
      return *this;
    }
  };

  UpCodeMotionPass() : Pass("UpCodeMotionPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static Stats process_code(bool is_static,
                            DexType* declaring_type,
                            DexTypeList* args,
                            IRCode*);

 private:
  static bool gather_movable_instructions(
      cfg::Block* b, std::vector<IRInstruction*>* instructions);
  static bool gather_instructions_to_insert(
      cfg::Edge* branch_edge,
      cfg::Edge* goto_edge,
      std::vector<IRInstruction*>* instructions_to_insert);
};
