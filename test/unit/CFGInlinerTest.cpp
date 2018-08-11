/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CFGInliner.h"
#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"

cfg::InstructionIterator get_invoke(cfg::ControlFlowGraph* cfg) {
  auto iterable = cfg::InstructionIterable(*cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (is_invoke(it->insn->opcode())) {
      return it;
    }
  }
  always_assert_log(false, "can't find invoke in %s", SHOW(*cfg));
}

void test_inliner(const std::string& caller_str,
                  const std::string& callee_str,
                  const std::string& expected_str) {
  g_redex = new RedexContext();

  auto caller_code = assembler::ircode_from_string(caller_str);
  caller_code->build_cfg(true);
  auto& caller = caller_code->cfg();

  auto callee_code = assembler::ircode_from_string(callee_str);
  callee_code->build_cfg(true);
  auto& callee = callee_code->cfg();

  cfg::CFGInliner::inline_cfg(&caller, get_invoke(&caller), callee);
  TRACE(CFG, 1, "%s\n", SHOW(caller));

  auto expected_code = assembler::ircode_from_string(expected_str);
  caller_code->clear_cfg();
  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(caller_code.get()));

  delete g_redex;
}

TEST(CFGInliner, simple) {
  const auto& caller_str = R"(
    (
      (invoke-static () "LCls;.foo:()V")
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (return-void)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST(CFGInliner, with_regs) {
  const auto& caller_str = R"(
    (
      (const v0 0)
      (invoke-static () "LCls;.foo:()V")
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (const v0 1)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 0)
      (const v1 1)
      (return-void)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST(CFGInliner, with_args) {
  const auto& caller_str = R"(
    (
      (const v0 0)
      (invoke-static (v0) "LCls;.foo:(I)V")
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 0)
      (move v1 v0)
      (const v2 1)
      (return-void)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST(CFGInliner, with_returns) {
  const auto& caller_str = R"(
    (
      (const v0 0)
      (invoke-static () "LCls;.foo:()I")
      (move-result v1)
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (const v0 1)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 0)
      (const v2 1)
      (move v1 v2)
      (return-void)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST(CFGInliner, with_args_and_returns) {
  const auto& caller_str = R"(
    (
      (const v0 0)
      (invoke-static (v0) "LCls;.foo:(I)I")
      (move-result v0)
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (add-int v0 v0 v1)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 0)
      (move v1 v0)
      (const v2 1)
      (add-int v1 v1 v2)
      (move v0 v1)
      (return-void)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST(CFGInliner, multi_return) {
  const auto& caller_str = R"(
    (
      (const v0 0)
      (const v1 10)
      (invoke-static (v0 v1) "LCls;.max:(II)I")
      (move-result v2)
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      ; max
      (load-param v0)
      (load-param v1)
      (if-ge v0 v1 :true)

      (return v1)

      (:true)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 0)
      (const v1 10)
      (move v3 v0)
      (move v4 v1)
      (if-ge v3 v4 :true)

      (move v2 v4)
      (goto :out)

      (:true)
      (move v2 v3)

      (:out)
      (return-void)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST(CFGInliner, both_multi_block) {
  const auto& caller_str = R"(
    (
      (const v0 0)
      (const v1 10)
      (if-nez v1 :true)
      (return-void)

      (:true)
      (invoke-static (v0 v1) "LCls;.max:(II)I")
      (move-result v2)
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      ; max
      (load-param v0)
      (load-param v1)
      (if-ge v0 v1 :true)

      (return v1)

      (:true)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 0)
      (const v1 10)
      (if-nez v1 :outer_true)
      (return-void)

      (:outer_true)
      (move v3 v0)
      (move v4 v1)
      (if-ge v3 v4 :true)

      (move v2 v4)
      (goto :out)

      (:true)
      (move v2 v3)

      (:out)
      (return-void)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST(CFGInliner, callee_diamond_caller_loop) {
  const auto& caller_str = R"(
    (
      (const v0 10)

      (:loop)
      (if-eqz v0 :end)
      (invoke-static (v0) "LCls;.foo:(I)I")
      (move-result v1)
      (add-int v0 v0 v1)
      (goto :loop)

      (:end)
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (load-param v0)
      (if-nez v0 :true)
      (const v0 0)
      (goto :end)

      (:true)
      (const v0 -1)

      (:end)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 10)

      (:loop)
      (if-eqz v0 :end)

      ; callee starts here
      (move v2 v0)
      (if-nez v2 :true)
      (const v2 0)
      (goto :inner_end)

      (:true)
      (const v2 -1)

      (:inner_end)
      (move v1 v2)
      ; callee ends here

      (add-int v0 v0 v1)
      (goto :loop)

      (:end)
      (return-void)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}
