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

#include "ResultPropagation.h"

boost::optional<ParamIndex> find_return_param_index(
    cfg::ControlFlowGraph& cfg) {
  for (auto& mie : InstructionIterable(cfg)) {
    TRACE(RP, 2, "  %s", SHOW(mie.insn));
  }
  // find register that is being returned (if any)
  cfg.calculate_exit_block();
  auto exit_block = cfg.exit_block();
  auto it = exit_block->rbegin();
  if (it == exit_block->rend() ||
      !opcode::is_a_return_value(it->insn->opcode()))
    return boost::none;
  auto return_reg = it->insn->src(0);
  TRACE(RP, 2, "  returns v%d", return_reg);
  ++it;
  if (it == exit_block->rend() || !opcode::is_a_move(it->insn->opcode()))
    return boost::none;
  auto src_reg = it->insn->src(0);
  TRACE(RP, 2, "  move v%d, v%d", it->insn->dest(), src_reg);
  if (it->insn->dest() != return_reg) return boost::none;
  // let's see if it came from a unique load-param
  IRInstruction* load_param = nullptr;
  for (auto& mie : InstructionIterable(cfg)) {
    if (mie.insn->has_dest()) {
      if (mie.insn->dest() == src_reg) {
        if (opcode::is_a_load_param(mie.insn->opcode())) {
          load_param = mie.insn;
        } else {
          TRACE(RP, 2, "  move_reg clobbered");
          return boost::none;
        }
      }
    }
  }
  if (load_param != nullptr) {
    ParamIndex param_index = get_load_param_map(cfg).at(load_param);
    TRACE(RP, 2, "  found matching load-param %d", param_index);
    return param_index;
  } else {
    TRACE(RP, 2, "  did not find matching load-param");
    return boost::none;
  }
}

class ResultPropagationTest : public RedexIntegrationTest {};

TEST_F(ResultPropagationTest, useSwitch) {
  std::vector<Pass*> passes = {
      new ResultPropagationPass(),
  };

  run_passes(passes);

  int num_test_classes = 0;
  const char* test_class_prefix = "Lcom/facebook/redextest/ResultPropagation$";
  const char* test_method_prefix = "returns_";
  for (const auto& cls : *classes) {
    if (strncmp(cls->get_name()->c_str(), test_class_prefix,
                strlen(test_class_prefix)) == 0) {
      TRACE(RP, 1, "test class %s", cls->get_name()->c_str());
      num_test_classes++;
      int num_tests_in_class = 0;
      for (const auto& m : cls->get_vmethods()) {
        const auto method_name = m->get_name()->c_str();
        TRACE(RP, 1, " test method %s", method_name);
        EXPECT_EQ(strncmp(method_name, test_method_prefix,
                          strlen(test_method_prefix)),
                  0);
        const auto suffix = method_name + strlen(test_method_prefix);
        boost::optional<ParamIndex> expected;
        if (strcmp(suffix, "none") != 0) {
          expected = atoi(suffix);
        }

        IRCode* code = m->get_code();
        code->build_cfg(/* editable */ true);
        auto actual = find_return_param_index(code->cfg());
        code->clear_cfg();
        if (expected) {
          EXPECT_TRUE(actual);
          EXPECT_EQ(*actual, *expected);
        } else {
          EXPECT_FALSE(actual);
        }
        num_tests_in_class++;
      }
      EXPECT_EQ(num_tests_in_class, 1);
    }
  }
  EXPECT_EQ(num_test_classes, 6);
}
