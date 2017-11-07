/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "ConstantPropagation.h"
#include "IRAssembler.h"

static void do_const_prop(IRCode* code, const ConstPropConfig& config) {
  code->build_cfg();
  IntraProcConstantPropagation rcp(code->cfg(), config);
  rcp.run(ConstPropEnvironment());
  rcp.simplify();
  rcp.apply_changes(code);
}

TEST(ConstantPropagation, IfToGoto) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 0)

     (if-eqz v0 :if-true-label)
     (const/4 v0 1)

     :if-true-label
     (const/4 v0 2)
    )
)");

  ConstPropConfig config;
  config.propagate_conditions = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 0)

     (goto :if-true-label)
     (const/4 v0 1)

     :if-true-label
     (const/4 v0 2)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstant_EqualsAlwaysTrue) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 0)
     (const/4 v1 0)

     (if-eqz v0 :if-true-label-1)
     (const/4 v1 1) ; the preceding opcode always jumps, so this is unreachable

     :if-true-label-1
     (if-eqz v1 :if-true-label-2) ; therefore this is always true
     (const/4 v1 2)

     :if-true-label-2
     (return-void)
    )
)");

  ConstPropConfig config;
  config.propagate_conditions = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 0)
     (const/4 v1 0)

     (goto :if-true-label-1)
     (const/4 v1 1)

     :if-true-label-1
     (goto :if-true-label-2)
     (const/4 v1 2)

     :if-true-label-2
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstant_EqualsAlwaysFalse) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 1)
     (const/4 v1 1)

     (if-eqz v0 :if-true-label-1)
     (const/4 v1 0) ; the preceding opcode never jumps, so this is always
                    ; executed
     :if-true-label-1
     (if-eqz v1 :if-true-label-2) ; therefore this is always true
     (const/4 v1 2)

     :if-true-label-2
     (return-void)
    )
)");

  ConstPropConfig config;
  config.propagate_conditions = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 1)
     (const/4 v1 1)

     (const/4 v1 0)

     (goto :if-true-label-2)
     (const/4 v1 2)

     :if-true-label-2
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstant_LessThanAlwaysTrue) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 0)
     (const/4 v1 1)

     (if-lt v0 v1 :if-true-label-1)
     (const/4 v1 0) ; the preceding opcode always jumps, so this is never
                    ; executed
     :if-true-label-1
     (if-eqz v1 :if-true-label-2) ; therefore this is never true
     (const/4 v1 2)

     :if-true-label-2
     (return-void)
    )
)");

  ConstPropConfig config;
  config.propagate_conditions = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 0)
     (const/4 v1 1)

     (goto :if-true-label-1)
     (const/4 v1 0)

     :if-true-label-1
     (const/4 v1 2)

     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstant_LessThanAlwaysFalse) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 1)
     (const/4 v1 0)

     (if-lt v0 v1 :if-true-label-1)
     (const/4 v0 0) ; the preceding opcode never jumps, so this is always
                    ; executed
     :if-true-label-1
     (if-eqz v0 :if-true-label-2) ; therefore this is always true
     (const/4 v1 2)

     :if-true-label-2
     (return-void)
    )
)");

  ConstPropConfig config;
  config.propagate_conditions = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 1)
     (const/4 v1 0)

     (const/4 v0 0)

     (goto :if-true-label-2)
     (const/4 v1 2)

     :if-true-label-2
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstantInferZero) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; some unknown value

     (if-nez v0 :exit)
     (if-eqz v0 :exit) ; we know v0 must be zero here, so this is always true

     (const/4 v0 1)

     :exit
     (return-void)
    )
)");

  ConstPropConfig config;
  config.propagate_conditions = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)

     (if-nez v0 :exit)
     (goto :exit)

     (const/4 v0 1)

     :exit
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, JumpToImmediateNext) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (if-eqz v0 :next) ; This jumps to the next opcode regardless of whether
                       ; the test is true or false. So in this case we cannot
                       ; conclude that v0 == 0 in the 'true' block, since that
                       ; is identical to the 'false' block.
     :next
     (if-eqz v0 :end)
     (const/4 v0 1)
     :end
     (return-void)
    )
)");

  ConstPropConfig config;
  config.propagate_conditions = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (if-eqz v0 :next)
     :next
     (if-eqz v0 :end)
     (const/4 v0 1)
     :end
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}
