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

TEST(ConstantPropagation, ConditionalConstantInferInterval) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; some unknown value

     (if-lez v0 :exit)
     (if-gtz v0 :exit) ; we know v0 must be > 0 here, so this is always true

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

     (if-lez v0 :exit)
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

TEST(ConstantPropagation, SignedConstantDomainOperations) {
  using namespace sign_domain;
  auto one = SignedConstantDomain(1, ConstantValue::ConstantType::NARROW);
  auto minus_one =
      SignedConstantDomain(-1, ConstantValue::ConstantType::NARROW);
  auto zero = SignedConstantDomain(0, ConstantValue::ConstantType::NARROW);
  auto max_val = SignedConstantDomain(std::numeric_limits<int64_t>::max(),
                                      ConstantValue::ConstantType::WIDE);
  auto min_val = SignedConstantDomain(std::numeric_limits<int64_t>::min(),
                                      ConstantValue::ConstantType::WIDE);

  EXPECT_EQ(one.interval(), Interval::GTZ);
  EXPECT_EQ(minus_one.interval(), Interval::LTZ);
  EXPECT_EQ(zero.interval(), Interval::EQZ);
  EXPECT_EQ(max_val.interval(), Interval::GTZ);
  EXPECT_EQ(min_val.interval(), Interval::LTZ);

  EXPECT_EQ(one.join(minus_one).interval(), Interval::ALL);
  EXPECT_EQ(one.join(zero).interval(), Interval::GEZ);
  EXPECT_EQ(minus_one.join(zero).interval(), Interval::LEZ);
  EXPECT_EQ(max_val.join(zero).interval(), Interval::GEZ);
  EXPECT_EQ(min_val.join(zero).interval(), Interval::LEZ);

  auto positive = SignedConstantDomain(Interval::GTZ);
  auto negative = SignedConstantDomain(Interval::LTZ);

  EXPECT_EQ(one.join(positive), positive);
  EXPECT_TRUE(one.join(negative).is_top());
  EXPECT_EQ(max_val.join(positive), positive);
  EXPECT_TRUE(max_val.join(negative).is_top());
  EXPECT_EQ(minus_one.join(negative), negative);
  EXPECT_TRUE(minus_one.join(positive).is_top());
  EXPECT_EQ(min_val.join(negative), negative);
  EXPECT_TRUE(min_val.join(positive).is_top());
  EXPECT_EQ(zero.join(positive).interval(), Interval::GEZ);
  EXPECT_EQ(zero.join(negative).interval(), Interval::LEZ);

  EXPECT_EQ(one.meet(positive), one);
  EXPECT_TRUE(one.meet(negative).is_bottom());
  EXPECT_EQ(max_val.meet(positive), max_val);
  EXPECT_TRUE(max_val.meet(negative).is_bottom());
  EXPECT_EQ(minus_one.meet(negative), minus_one);
  EXPECT_TRUE(minus_one.meet(positive).is_bottom());
  EXPECT_EQ(min_val.meet(negative), min_val);
  EXPECT_TRUE(min_val.meet(positive).is_bottom());
}
