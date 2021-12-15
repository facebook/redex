/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "AbstractDomainPropertyTest.h"
#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "SignedConstantDomain.h"

struct Constants {
  SignedConstantDomain one{SignedConstantDomain(1)};

  SignedConstantDomain minus_one{SignedConstantDomain(-1)};

  SignedConstantDomain zero{SignedConstantDomain(0)};

  SignedConstantDomain max_val{
      SignedConstantDomain(std::numeric_limits<int64_t>::max())};

  SignedConstantDomain min_val{
      SignedConstantDomain(std::numeric_limits<int64_t>::min())};

  SignedConstantDomain positive{
      SignedConstantDomain(sign_domain::Interval::GTZ)};

  SignedConstantDomain negative{
      SignedConstantDomain(sign_domain::Interval::LTZ)};

  SignedConstantDomain not_zero{
      SignedConstantDomain(sign_domain::Interval::NEZ)};
};

INSTANTIATE_TYPED_TEST_CASE_P(SignedConstantDomain,
                              AbstractDomainPropertyTest,
                              SignedConstantDomain);

template <>
std::vector<SignedConstantDomain>
AbstractDomainPropertyTest<SignedConstantDomain>::non_extremal_values() {
  Constants constants;
  return {constants.one,      constants.minus_one, constants.zero,
          constants.max_val,  constants.min_val,   constants.positive,
          constants.negative, constants.not_zero};
}

class SignedConstantDomainOperationsTest : public testing::Test,
                                           public Constants {};

TEST_F(SignedConstantDomainOperationsTest, intervals) {
  using namespace sign_domain;

  EXPECT_EQ(one.interval(), Interval::GTZ);
  EXPECT_EQ(minus_one.interval(), Interval::LTZ);
  EXPECT_EQ(zero.interval(), Interval::EQZ);
  EXPECT_EQ(SignedConstantDomain(Interval::EQZ), zero);
  EXPECT_EQ(max_val.interval(), Interval::GTZ);
  EXPECT_EQ(min_val.interval(), Interval::LTZ);
  EXPECT_EQ(not_zero.interval(), Interval::NEZ);

  EXPECT_EQ(one.join(minus_one).interval(), Interval::NEZ);
  EXPECT_EQ(one.join(zero).interval(), Interval::GEZ);
  EXPECT_EQ(minus_one.join(zero).interval(), Interval::LEZ);
  EXPECT_EQ(max_val.join(zero).interval(), Interval::GEZ);
  EXPECT_EQ(min_val.join(zero).interval(), Interval::LEZ);
  EXPECT_EQ(min_val.join(max_val).interval(), Interval::NEZ);
}

TEST_F(SignedConstantDomainOperationsTest, numeric_intervals) {
  using namespace sign_domain;

  EXPECT_EQ(one.numeric_interval_domain(), NumericIntervalDomain::finite(1, 1));
  EXPECT_EQ(minus_one.numeric_interval_domain(),
            NumericIntervalDomain::finite(-1, -1));
  EXPECT_EQ(zero.numeric_interval_domain(),
            NumericIntervalDomain::finite(0, 0));
  EXPECT_EQ(NumericIntervalDomain::finite(0, 0),
            zero.numeric_interval_domain());
  EXPECT_EQ(max_val.numeric_interval_domain(), NumericIntervalDomain::high());
  EXPECT_EQ(min_val.numeric_interval_domain(), NumericIntervalDomain::low());
  EXPECT_EQ(not_zero.numeric_interval_domain(), NumericIntervalDomain::top());

  EXPECT_EQ(one.join(minus_one).numeric_interval_domain(),
            NumericIntervalDomain::finite(-1, 1));
  EXPECT_EQ(one.join(zero).numeric_interval_domain(),
            NumericIntervalDomain::finite(0, 1));
  EXPECT_EQ(minus_one.join(zero).numeric_interval_domain(),
            NumericIntervalDomain::finite(-1, 0));
  EXPECT_EQ(max_val.join(zero).numeric_interval_domain(),
            NumericIntervalDomain::bounded_below(0));
  EXPECT_EQ(min_val.join(zero).numeric_interval_domain(),
            NumericIntervalDomain::bounded_above(0));
  EXPECT_EQ(min_val.join(max_val).numeric_interval_domain(),
            NumericIntervalDomain::top());
}

TEST_F(SignedConstantDomainOperationsTest, binaryOperations) {
  using namespace sign_domain;

  EXPECT_EQ(one.join(positive), positive);
  EXPECT_EQ(max_val.join(positive), positive);
  EXPECT_EQ(minus_one.join(negative), negative);
  EXPECT_EQ(min_val.join(negative), negative);
  EXPECT_EQ(zero.join(positive).interval(), Interval::GEZ);
  EXPECT_EQ(zero.join(negative).interval(), Interval::LEZ);
  EXPECT_EQ(zero.join(not_zero).interval(), Interval::ALL);

  EXPECT_EQ(one.meet(positive), one);
  EXPECT_TRUE(one.meet(negative).is_bottom());
  EXPECT_EQ(max_val.meet(positive), max_val);
  EXPECT_TRUE(max_val.meet(negative).is_bottom());
  EXPECT_EQ(minus_one.meet(negative), minus_one);
  EXPECT_TRUE(minus_one.meet(positive).is_bottom());
  EXPECT_EQ(min_val.meet(negative), min_val);
  EXPECT_TRUE(min_val.meet(positive).is_bottom());
  EXPECT_TRUE(zero.meet(not_zero).is_bottom());
  EXPECT_EQ(not_zero.meet(positive), positive);
  EXPECT_EQ(not_zero.meet(max_val), max_val);
  EXPECT_EQ(not_zero.meet(min_val), min_val);
}

class ConstantNezTest : public RedexTest {};

TEST_F(ConstantNezTest, DeterminableNezTrue) {
  auto code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (invoke-direct (v0) "LFoo;.<init>:()V")

     (if-nez v0 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (invoke-direct (v0) "LFoo;.<init>:()V")

     (const v0 2)

     (return-void)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantNezTest, DeterminableNezFalse) {
  auto code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (const v0 0)

     (if-nez v0 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (const v0 0)

     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantNezTest, DeterminableEZFalse) {
  auto code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)

     (if-eqz v0 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)

     (const v0 1)

     (const v0 2)

     (return-void)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantNezTest, NonDeterminableNEZ) {
  auto code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (invoke-direct (v0) "LFoo;.<init>:()V")
     (iget v0 "LBoo;.a:I")
     (move-result-pseudo v0)

     (if-nez v0 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (invoke-direct (v0) "LFoo;.<init>:()V")
     (iget v0 "LBoo;.a:I")
     (move-result-pseudo v0)

     (if-nez v0 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, IfToGoto) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)

     (if-eqz v0 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)

     (const v0 2)

     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, FoldArithmeticAddLit) {
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
     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, AnalyzeCmp) {
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

      (:end)
      (return v2)

      (:b2)
      (const-wide v0 1)
      (const-wide v1 0)
      (cmp-long v2 v0 v1)
      (const v3 1)
      (goto :end)

      (:b1)
      (const-wide v0 1)
      (const-wide v1 1)
      (cmp-long v2 v0 v1)
      (const v3 0)
      (goto :end)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ConditionalConstant_EqualsAlwaysTrue) {
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

     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ConditionalConstant_EqualsAlwaysFalse) {
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

     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ConditionalConstant_LessThanAlwaysTrue) {
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

     (const v1 2)

     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ConditionalConstant_LessThanAlwaysFalse) {
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

     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ConditionalConstantInferZero) {
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

     (:exit)
     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ConditionalConstantInferInterval) {
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

     (:exit)
     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ConditionalConstantCompareIntervals) {
  auto code = assembler::ircode_from_string(R"( (
       (load-param v0)
       (load-param v1)

       (if-gtz v0 :if-gtz-label)
       ; here v0 is <= 0
       (if-ltz v1 :if-ltz-label)
       ; here v1 is >= 0
       (if-le v0 v1 :exit)

       (const v3 0)
       (:if-gtz-label)
       (const v4 0)
       (:if-ltz-label)
       (const v5 0)
       (:exit)
       (return-void)
      )
  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"( (
       (load-param v0)
       (load-param v1)

       (if-gtz v0 :if-gtz-label)
       ; here v0 is <= 0
       (if-ltz v1 :if-ltz-label)
       ; here v1 is >= 0

       (:exit)
       (return-void)

       (:if-gtz-label)
       (const v4 0)
       (:if-ltz-label)
       (const v5 0)
       (goto :exit)
      )
  )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

// This test catches the regression described in D8676637.
TEST_F(ConstantPropagationTest, MayMustCompare) {
  {
    auto code = assembler::ircode_from_string(R"( (
       (load-param v0)
       (load-param v1)

       (if-gtz v0 :if-gtz-label)
       ; here v0 is <= 0
       (if-ltz v1 :if-ltz-label)
       ; here v1 is >= 0

       (const v2 0)
       ; v0 < v1 may not be true since v0 == v1 is possible
       (if-lt v0 v1 :if-lt-label)
       (const v3 0)
       (:if-gtz-label)
       (const v4 0)
       (:if-ltz-label)
       (const v5 0)
       (:if-lt-label)
       (return-void)
      )
  )");
    auto expected = assembler::to_s_expr(code.get());
    do_const_prop(code.get());
    EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
  }

  {
    auto code = assembler::ircode_from_string(R"( (
       (load-param v0)
       (load-param v1)

       (if-gtz v0 :if-gtz-label)
       ; here v0 is <= 0
       (if-ltz v1 :if-ltz-label)
       ; here v1 is >= 0

       (const v2 0)
       ; v1 > v0 may not be true since v0 == v1 is possible
       (if-gt v1 v0 :if-gt-label)
       (const v3 0)
       (:if-gtz-label)
       (const v4 0)
       (:if-ltz-label)
       (const v5 0)
       (:if-gt-label)
       (return-void)
      )
  )");
    auto expected = assembler::to_s_expr(code.get());
    do_const_prop(code.get());
    EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
  }
}

TEST_F(ConstantPropagationTest, FoldBitwiseAndLit) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1023)
      (and-int/lit16 v0 v0 511)
      (and-int/lit8 v0 v0 255)
      (return-void)
    )
  )");
  do_const_prop(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1023)
      (const v0 511)
      (const v0 255)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, FoldBitwiseOrLit) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 257)
      (or-int/lit8 v0 v0 255)
      (or-int/lit16 v0 v0 1024)
      (return-void)
    )
  )");
  do_const_prop(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 257)
      (const v0 511)
      (const v0 1535)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, FoldBitwiseXorLit) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1023)
      (xor-int/lit16 v0 v0 512)
      (xor-int/lit8 v0 v0 255)
      (return-void)
    )
  )");
  do_const_prop(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1023)
      (const v0 511)
      (const v0 256)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, FoldBitwiseShiftLeftOverflowLit) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -16776961)
      (shl-int/lit8 v0 v0 8)
      (return-void)
    )
  )");
  do_const_prop(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -16776961)
      (const v0 65280)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, FoldBitwiseShiftLit) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1023)
      (shr-int/lit8 v0 v0 2)
      (shl-int/lit8 v0 v0 1)
      (return-void)
    )
  )");
  do_const_prop(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1023)
      (const v0 255)
      (const v0 510)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, FoldBitwiseOverShiftLit) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1023)
      (shr-int/lit8 v0 v0 34)
      (shl-int/lit8 v0 v0 33)
      (return-void)
    )
  )");
  do_const_prop(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1023)
      (const v0 255)
      (const v0 510)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, FoldBitwiseArithAndLogicalRightShiftLit) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -1024)
      (shr-int/lit8 v0 v0 2)
      (ushr-int/lit8 v0 v0 12)
      (return-void)
    )
  )");
  do_const_prop(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -1024)
      (const v0 -256)
      (const v0 1048575)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, FoldDivIntLit) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 4096)
      (div-int/lit16 v0 512)
      (move-result-pseudo v1)
      (const v0 15)
      (div-int/lit8 v0 2)
      (move-result-pseudo v1)
      (div-int/lit8 v0 0)
      (move-result-pseudo v2)
      (return-void)
    )
  )");
  do_const_prop(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 4096)
      (const v1 8)
      (const v0 15)
      (const v1 7)
      (div-int/lit8 v0 0)
      (move-result-pseudo v2)
      (return-void)
    )
  )"); // division by 0 should not be optimized out
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
