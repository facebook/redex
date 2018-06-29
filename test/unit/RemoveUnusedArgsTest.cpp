/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iterator>

#include "ControlFlow.h"
#include "Creators.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RemoveUnusedArgs.h"

struct RemoveUnusedArgsTest : testing::Test {
  remove_unused_args::RemoveArgs* m_remove_args;

  RemoveUnusedArgsTest() {
    g_redex = new RedexContext();
    Scope dummy_scope;
    m_remove_args = new remove_unused_args::RemoveArgs(dummy_scope);
  }

   ~RemoveUnusedArgsTest() { delete g_redex; }
};

// Checks argument liveness on a method with no arguments
TEST_F(RemoveUnusedArgsTest, noArgs) {
  // no args alive
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:()V"
      (
        (const v0 0)
        (mul-int v0 v0 v0)
        (return-void)
      )
    )
  )");

  auto code = method->get_code();
  auto live_arg_regs = m_remove_args->compute_live_arg_regs(code);
  EXPECT_THAT(live_arg_regs, ::testing::ElementsAre());
}

// Checks liveness on methods with a single used argument
TEST_F(RemoveUnusedArgsTest, simpleUsedArg) {
  // only v_1 alive
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(D)V"
      (
        (load-param v1)
        (add-int v0 v1 v1)
        (return-wide v1)
      )
    )
  )");

  auto code = method->get_code();
  auto live_arg_regs = m_remove_args->compute_live_arg_regs(code);
  EXPECT_THAT(live_arg_regs, ::testing::ElementsAre(1));
}

// Checks liveness on methods with a single used WIDE argument
TEST_F(RemoveUnusedArgsTest, simpleUsedArgWide) {
  // only 0_v alive
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(D)V"
      (
        (load-param-wide v0)
        (return-wide v0)
      )
    )
  )");

  auto code = method->get_code();
  auto live_arg_regs = m_remove_args->compute_live_arg_regs(code);
  EXPECT_EQ(live_arg_regs.size(), 1);
}

// Checks liveness on methods with multiple args, not wide
TEST_F(RemoveUnusedArgsTest, simpleUsedArgs) {
  // only 3_v, 5_v alive
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(III)V"
      (
        (load-param v3)
        (load-param v4)
        (load-param v5)
        (add-int v1 v3 v5)
        (add-int v3 v3 v5)
        (return-void)
      )
    )
  )");

  auto code = method->get_code();
  auto live_arg_regs = m_remove_args->compute_live_arg_regs(code);
  EXPECT_THAT(live_arg_regs, ::testing::ElementsAre(3, 5));
}

// Checks liveness on methods with multiple wide args
TEST_F(RemoveUnusedArgsTest, simpleUsedArgsWide) {
  // only 3_v, 5_v alive
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(DDD)V"
      (
        (load-param-wide v3)
        (load-param-wide v5)
        (load-param-wide v7)
        (invoke-static (v3 v5) "Lfoo;.baz:(DD)V")
        (return-void)
      )
    )
  )");

  auto code = method->get_code();
  auto live_arg_regs = m_remove_args->compute_live_arg_regs(code);
  EXPECT_THAT(live_arg_regs, ::testing::ElementsAre(3, 5));
}

// Checks liveness on methods with multiple blocks, only default sized args
TEST_F(RemoveUnusedArgsTest, multipleBlocksRegularArgs) {
  // all regs 2_v, 3_v, 4_v alive
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(III)V"
      (
        (load-param v2)
        (load-param v3)
        (load-param v4)
        (if-eqz v0 :left)
        (goto :right)

        (:left)
        (add-int v3 v2 v4) ; kills v3, marks v2 and v4 live
        (goto :middle)

        (:right)
        (add-int v3 v3 v3) : marks v3 live

        (:middle)
        (return-void)
      )
    )
  )");

  auto code = method->get_code();
  auto live_arg_regs = m_remove_args->compute_live_arg_regs(code);
  std::vector<uint16_t> expected_live_regs{2, 3, 4};
  EXPECT_THAT(live_arg_regs, ::testing::ElementsAre(2, 3, 4));
}

// Checks liveness on methods with multiple blocks, only wide sized args
TEST_F(RemoveUnusedArgsTest, multipleBlocksWideArgs) {
  // all regs 2_v, 4_v, 6_v alive
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(DDD)V"
      (
        (load-param-wide v2)
        (load-param-wide v4)
        (load-param-wide v6)
        (if-eqz v0 :left)
        (goto :right)

        (:left)
        (add-double v6 v2 v4) ; kills v6, marks v2 and v4 live
        (goto :middle)

        (:right)
        (add-double v6 v6 v6) : marks v6 live

        (:middle)
        (return-void)
      )
    )
  )");

  auto code = method->get_code();
  auto live_arg_regs = m_remove_args->compute_live_arg_regs(code);
  EXPECT_THAT(live_arg_regs, ::testing::ElementsAre(2, 4, 6));
}

// Checks liveness on methods with multiple blocks, mixed size args
TEST_F(RemoveUnusedArgsTest, multipleBlocksMixedArgs) {
  // regs 2_v, 4_v, 5_v, 7_v
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(DIDI)V"
      (
        (load-param-wide v2)
        (load-param v4)
        (load-param-wide v5)
        (load-param v7)
        (if-eqz v4 :left) ; marks v4 live
        (goto :right)

        (:left)
        (add-double v5 v2 v2) ; kills v5, marks v2 live
        (goto :middle)

        (:right)
        (add-double v5 v0 v5) ; marks v5 live
        (invoke-static (v7) "Lfoo;.baz:(D)V") ; marks v7 live

        (:middle)
        (return-void)
      )
    )
  )");

  auto code = method->get_code();
  auto live_arg_regs = m_remove_args->compute_live_arg_regs(code);
  EXPECT_THAT(live_arg_regs, ::testing::ElementsAre(2, 4, 5, 7));
}
