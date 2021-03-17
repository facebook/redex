/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "Show.h"
#include "Trace.h"
#include "Transform.h"

#include "DedupBlocksPass.h"

int count_someFunc_calls(cfg::ControlFlowGraph& cfg) {
  int num_some_func_calls = 0;
  for (auto& mie : InstructionIterable(cfg)) {
    TRACE(DEDUP_BLOCKS, 1, "%s", SHOW(mie.insn));
    if (mie.insn->has_method()) {
      DexMethodRef* called = mie.insn->get_method();
      if (strcmp(called->get_name()->c_str(), "someFunc") == 0) {
        num_some_func_calls++;
      }
    }
  }
  return num_some_func_calls;
}

class DedupBlocksTest : public RedexIntegrationTest {};

TEST_F(DedupBlocksTest, useSwitch) {
  bool code_checked_before = false;
  TRACE(DEDUP_BLOCKS, 1, "Code before:");
  for (const auto& cls : *classes) {
    TRACE(DEDUP_BLOCKS, 1, "Class %s", SHOW(cls));
    for (const auto& m : cls->get_vmethods()) {
      if (strcmp(m->get_name()->c_str(), "useSwitch") == 0) {
        code_checked_before = true;
        IRCode* code = m->get_code();
        code->build_cfg(/* editable */ true);
        EXPECT_EQ(count_someFunc_calls(code->cfg()), 3);
        code->clear_cfg();
      }
    }
  }
  EXPECT_TRUE(code_checked_before);

  std::vector<Pass*> passes = {
      new DedupBlocksPass(),
  };

  run_passes(passes);

  bool code_checked_after = false;
  TRACE(DEDUP_BLOCKS, 1, "Code after:");
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (strcmp(m->get_name()->c_str(), "useSwitch") == 0) {
        code_checked_after = true;
        IRCode* code = m->get_code();
        code->build_cfg(/* editable */ true);
        EXPECT_EQ(count_someFunc_calls(code->cfg()), 1);
        code->clear_cfg();
      }
    }
  }
  EXPECT_TRUE(code_checked_after);
}
