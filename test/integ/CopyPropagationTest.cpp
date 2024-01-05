/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "Show.h"
#include "Transform.h"

#include "CopyPropagationPass.h"

int count_sgets(cfg::ControlFlowGraph& cfg) {
  int sgets = 0;
  for (auto& mie : InstructionIterable(cfg)) {
    TRACE(RME, 1, "%s", SHOW(mie.insn));
    if (opcode::is_an_sget(mie.insn->opcode())) {
      sgets++;
    }
  }
  return sgets;
}

class CopyPropagationTest : public RedexIntegrationTest {};

TEST_F(CopyPropagationTest, useSwitch) {
  TRACE(RME, 1, "Code before:");
  for (const auto& cls : *classes) {
    TRACE(RME, 1, "Class %s", SHOW(cls));
    for (const auto& m : cls->get_vmethods()) {
      TRACE(RME, 1, "\nmethod %s:", SHOW(m));
      IRCode* code = m->get_code();
      code->build_cfg();
      EXPECT_EQ(2, count_sgets(code->cfg()));
      code->clear_cfg();
    }
  }

  auto copy_prop = new CopyPropagationPass();
  copy_prop->m_config.static_finals = true;
  std::vector<Pass*> passes = {copy_prop};
  run_passes(passes);

  TRACE(RME, 1, "Code after:");
  for (const auto& cls : *classes) {
    for (const auto& m : cls->get_vmethods()) {
      TRACE(RME, 1, "\nmethod %s:", SHOW(m));
      IRCode* code = m->get_code();
      code->build_cfg();
      if (strcmp(m->get_name()->c_str(), "remove") == 0) {
        EXPECT_EQ(1, count_sgets(code->cfg()));
      } else {
        EXPECT_EQ(2, count_sgets(code->cfg()));
      }
      code->clear_cfg();
    }
  }
}
