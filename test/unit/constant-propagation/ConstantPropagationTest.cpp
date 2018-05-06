/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagation.h"

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

TEST(ConstantPropagation, JumpToImmediateNext) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (if-eqz v0 :next) ; This jumps to the next opcode regardless of whether
                       ; the test is true or false. So in this case we cannot
                       ; conclude that v0 == 0 in the 'true' block, since that
                       ; is identical to the 'false' block.
     (:next)
     (if-eqz v0 :end)
     (const v0 1)
     (:end)
     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (if-eqz v0 :next)
     (:next)
     (if-eqz v0 :end)
     (const v0 1)
     (:end)
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, WhiteBox1) {
  auto code = assembler::ircode_from_string(R"( (
     (load-param v0)

     (const v1 0)
     (const v2 1)
     (move v3 v1)
     (if-eqz v0 :if-true-label)

     (const v2 0)
     (if-gez v0 :if-true-label)

     (:if-true-label)
     (return-void)
    )
)");

  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  cp::intraprocedural::FixpointIterator intra_cp(
      cfg, cp::ConstantPrimitiveAnalyzer());
  intra_cp.run(ConstantEnvironment());

  auto exit_state = intra_cp.get_exit_state_at(cfg.exit_block());
  // Specifying `0u` here to avoid any ambiguity as to whether it is the null
  // pointer
  EXPECT_EQ(exit_state.get_primitive(0u), SignedConstantDomain::top());
  EXPECT_EQ(exit_state.get_primitive(1), SignedConstantDomain(0));
  // v2 can contain either the value 0 or 1
  EXPECT_EQ(exit_state.get_primitive(2),
            SignedConstantDomain(sign_domain::Interval::GEZ));
  EXPECT_EQ(exit_state.get_primitive(3), SignedConstantDomain(0));
}

TEST(ConstantPropagation, WhiteBox2) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)

     (:loop)
     (const v1 0)
     (if-gez v0 :if-true-label)
     (goto :loop)
     ; if we get here, that means v0 >= 0

     (:if-true-label)
     (return-void)
    )
)");

  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  cp::intraprocedural::FixpointIterator intra_cp(
      cfg, cp::ConstantPrimitiveAnalyzer());
  intra_cp.run(ConstantEnvironment());

  auto exit_state = intra_cp.get_exit_state_at(cfg.exit_block());
  EXPECT_EQ(exit_state.get_primitive(0u),
            SignedConstantDomain(sign_domain::Interval::GEZ));
  EXPECT_EQ(exit_state.get_primitive(1), SignedConstantDomain(0));
}
