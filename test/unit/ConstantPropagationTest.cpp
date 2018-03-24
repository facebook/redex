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

#include "ConstantPropagation.h"
#include "ConstantPropagationWholeProgramState.h"
#include "IRAssembler.h"
#include "RedexTest.h"

namespace cp = constant_propagation;

using Config = cp::intraprocedural::FixpointIterator::Config;

struct ConstantPropagationTest : public RedexTest {};

static void do_const_prop(IRCode* code, Config analysis_config = Config()) {
  code->build_cfg();
  analysis_config.fold_arithmetic = true;
  cp::intraprocedural::FixpointIterator rcp(code->cfg(), analysis_config);
  rcp.run(ConstantEnvironment());
  cp::Transform::Config transform_config;
  cp::Transform tf(transform_config);
  tf.apply(rcp, cp::WholeProgramState(), code);
}

TEST(ConstantPropagation, IfToGoto) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)

     (if-eqz v0 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)

     (goto :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstant_EqualsAlwaysTrue) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 0)

     (if-eqz v0 :if-true-label-1)
     (const v1 1) ; the preceding opcode always jumps, so this is unreachable

     (:if-true-label-1)
     (if-eqz v1 :if-true-label-2) ; therefore this is always true
     (const v1 2)

     (:if-true-label-2)
     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 0)

     (goto :if-true-label-1)
     (const v1 1)

     (:if-true-label-1)
     (goto :if-true-label-2)
     (const v1 2)

     (:if-true-label-2)
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstant_EqualsAlwaysFalse) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (const v1 1)

     (if-eqz v0 :if-true-label-1)
     (const v1 0) ; the preceding opcode never jumps, so this is always
                    ; executed
     (:if-true-label-1)
     (if-eqz v1 :if-true-label-2) ; therefore this is always true
     (const v1 2)

     (:if-true-label-2)
     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (const v1 1)

     (const v1 0)

     (goto :if-true-label-2)
     (const v1 2)

     (:if-true-label-2)
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstant_LessThanAlwaysTrue) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)

     (if-lt v0 v1 :if-true-label-1)
     (const v1 0) ; the preceding opcode always jumps, so this is never
                    ; executed
     (:if-true-label-1)
     (if-eqz v1 :if-true-label-2) ; therefore this is never true
     (const v1 2)

     (:if-true-label-2)
     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)

     (goto :if-true-label-1)
     (const v1 0)

     (:if-true-label-1)
     (const v1 2)

     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, ConditionalConstant_LessThanAlwaysFalse) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (const v1 0)

     (if-lt v0 v1 :if-true-label-1)
     (const v0 0) ; the preceding opcode never jumps, so this is always
                    ; executed
     (:if-true-label-1)
     (if-eqz v0 :if-true-label-2) ; therefore this is always true
     (const v1 2)

     (:if-true-label-2)
     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (const v1 0)

     (const v0 0)

     (goto :if-true-label-2)
     (const v1 2)

     (:if-true-label-2)
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

     (const v0 1)

     (:exit)
     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)

     (if-nez v0 :exit)
     (goto :exit)

     (const v0 1)

     (:exit)
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

     (const v0 1)

     (:exit)
     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)

     (if-lez v0 :exit)
     (goto :exit)

     (const v0 1)

     (:exit)
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

TEST(ConstantPropagation, FoldArithmeticAddLit) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 2147483646)
     (add-int/lit8 v0 v0 1) ; this should be converted to a const opcode
     (const v1 2147483647)
     (if-eq v0 v1 :end)
     (const v0 2147483647)
     (add-int/lit8 v0 v0 1) ; we don't handle overflows, so this should be
                            ; unchanged
     (:end)
     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 2147483646)
     (const v0 2147483647)
     (const v1 2147483647)
     (goto :end)
     (const v0 2147483647)
     (add-int/lit8 v0 v0 1)
     (:end)
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, AnalyzeCmp) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :b1) ; make sure all blocks appear reachable to constprop
      (if-gez v0 :b2)

      (:b0) ; case v0 < v1
      (const-wide v0 0)
      (const-wide v1 1)
      (cmp-long v2 v0 v1)
      (const v3 -1)
      (if-eq v2 v3 :end)

      (:b1) ; case v0 == v1
      (const-wide v0 1)
      (const-wide v1 1)
      (cmp-long v2 v0 v1)
      (const v3 0)
      (if-eq v2 v3 :end)

      (:b2) ; case v0 > v1
      (const-wide v0 1)
      (const-wide v1 0)
      (cmp-long v2 v0 v1)
      (const v3 1)
      (if-eq v2 v3 :end)

      (:end)
      (return v2)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :b1)
      (if-gez v0 :b2)

      (:b0)
      (const-wide v0 0)
      (const-wide v1 1)
      (cmp-long v2 v0 v1)
      (const v3 -1)
      (goto :end)

      (:b1)
      (const-wide v0 1)
      (const-wide v1 1)
      (cmp-long v2 v0 v1)
      (const v3 0)
      (goto :end)

      (:b2)
      (const-wide v0 1)
      (const-wide v1 0)
      (cmp-long v2 v0 v1)
      (const v3 1)
      (goto :end)

      (:end)
      (return v2)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(ConstantPropagation, SignedConstantDomainOperations) {
  using namespace sign_domain;
  auto one = SignedConstantDomain(1);
  auto minus_one = SignedConstantDomain(-1);
  auto zero = SignedConstantDomain(0);
  auto max_val = SignedConstantDomain(std::numeric_limits<int64_t>::max());
  auto min_val = SignedConstantDomain(std::numeric_limits<int64_t>::min());

  EXPECT_EQ(one.interval(), Interval::GTZ);
  EXPECT_EQ(minus_one.interval(), Interval::LTZ);
  EXPECT_EQ(zero.interval(), Interval::EQZ);
  EXPECT_EQ(SignedConstantDomain(Interval::EQZ), zero);
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

TEST_F(ConstantPropagationTest, ConstantArrayOperations) {
  {
    // Top cannot be changed to another value by setting an array index
    ConstantArrayDomain<SignedConstantDomain> arr;
    EXPECT_TRUE(arr.is_top());
    arr.set(0, SignedConstantDomain(1));
    EXPECT_TRUE(arr.is_top());
  }

  {
    // Arrays are zero-initialized
    ConstantArrayDomain<SignedConstantDomain> arr(10);
    EXPECT_EQ(arr.length(), 10);
    for (uint32_t i = 0; i < arr.length(); ++i) {
      EXPECT_EQ(arr.get(i), SignedConstantDomain(0));
    }
    // Check that iterating over the bindings works too
    size_t count = 0;
    for (auto& pair : arr.bindings()) {
      EXPECT_EQ(pair.second, SignedConstantDomain(0));
      ++count;
    }
    EXPECT_EQ(count, 10);
  }

  {
    // OOB read/write
    for (uint32_t i = 0; i < 10; ++i) {
      ConstantArrayDomain<SignedConstantDomain> arr(i);
      EXPECT_EQ(arr.length(), i);
      EXPECT_TRUE(arr.get(i).is_bottom());
      arr.set(i, SignedConstantDomain(1));
      EXPECT_TRUE(arr.is_bottom());
      EXPECT_THROW(arr.length(), std::runtime_error);
    }
  }

  {
    // join/meet of differently-sized arrays is Top/Bottom respectively
    ConstantArrayDomain<SignedConstantDomain> arr1(1);
    ConstantArrayDomain<SignedConstantDomain> arr2(2);
    EXPECT_TRUE(arr1.join(arr2).is_top());
    EXPECT_TRUE(arr1.meet(arr2).is_bottom());
  }
}

TEST_F(ConstantPropagationTest, PrimitiveArray) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (aput v1 v2 v0) ; write 1 into arr[0]
     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)
    )
)");

  Config config;
  config.analyze_arrays = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I")
     (move-result-pseudo-object v2)
     (aput v1 v2 v0)
     (const v3 1)

     (goto :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(ConstantPropagationTest, PrimitiveArrayAliased) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (move-object v3 v2) ; create an alias
     (aput v1 v3 v0) ; write 1 into arr[0]
     (aget v2 v0)
     (move-result-pseudo v4)

     (if-nez v4 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)
    )
)");

  auto expected = assembler::to_s_expr(code.get());
  Config config;
  config.analyze_arrays = true;
  do_const_prop(code.get(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I")
     (move-result-pseudo-object v2)
     (move-object v3 v2)
     (aput v1 v3 v0)
     (const v4 1)

     (goto :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(ConstantPropagationTest, PrimitiveArrayEscapesViaCall) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (aput v1 v2 v0) ; write 1 into arr[0]
     (invoke-static (v2) "LFoo;.bar:([I)V") ; bar() might modify the array
     (aget v2 v0)
     (move-result-pseudo v3)

     (if-eqz v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)
    )
)");

  auto expected = assembler::to_s_expr(code.get());
  Config config;
  config.analyze_arrays = true;
  do_const_prop(code.get(), config);
  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}

TEST_F(ConstantPropagationTest, PrimitiveArrayEscapesViaPut) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (aput v1 v2 v0) ; write 1 into arr[0]
     (move-object v3 v2) ; create an alias
     (sput-object v3 "LFoo;.bar:[I") ; write the array to a field via the alias
     (aget v2 v0)
     (move-result-pseudo v3)

     (if-eqz v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)
    )
)");

  auto expected = assembler::to_s_expr(code.get());
  Config config;
  config.analyze_arrays = true;
  do_const_prop(code.get(), config);
  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}

TEST_F(ConstantPropagationTest, OutOfBoundsWrite) {
  auto code = assembler::ircode_from_string(R"( (
     (const v0 1)
     (new-array v0 "[I") ; create an array of length 1
     (move-result-pseudo-object v1)
     (aput v0 v1 v0) ; write 1 into arr[1]
     (return-void)
    )
)");

  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  Config config;
  config.analyze_arrays = true;
  cp::intraprocedural::FixpointIterator rcp(cfg, config);
  rcp.run(ConstantEnvironment());
  EXPECT_TRUE(rcp.get_exit_state_at(cfg.exit_block()).is_bottom());
}

TEST_F(ConstantPropagationTest, OutOfBoundsRead) {
  auto code = assembler::ircode_from_string(R"( (
     (const v0 1)
     (new-array v0 "[I") ; create an array of length 1
     (move-result-pseudo-object v1)
     (aget v1 v0) ; read from arr[1]
     (move-result-pseudo v0)
     (return-void)
    )
)");

  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  Config config;
  config.analyze_arrays = true;
  cp::intraprocedural::FixpointIterator rcp(cfg, config);
  rcp.run(ConstantEnvironment());
  EXPECT_TRUE(rcp.get_exit_state_at(cfg.exit_block()).is_bottom());
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
  cp::intraprocedural::FixpointIterator rcp(cfg);
  rcp.run(ConstantEnvironment());

  auto exit_state = rcp.get_exit_state_at(cfg.exit_block());
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
  cp::intraprocedural::FixpointIterator rcp(cfg);
  rcp.run(ConstantEnvironment());

  auto exit_state = rcp.get_exit_state_at(cfg.exit_block());
  EXPECT_EQ(exit_state.get_primitive(0u),
            SignedConstantDomain(sign_domain::Interval::GEZ));
  EXPECT_EQ(exit_state.get_primitive(1), SignedConstantDomain(0));
}
