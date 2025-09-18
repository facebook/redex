/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "AbstractDomainPropertyTest.h"
#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

struct SignedConstantDomainZero {
  SignedConstantDomain operator()() { return SignedConstantDomain(0); }
};
using PrimitiveArrayDomain =
    ConstantArrayDomain<SignedConstantDomain, SignedConstantDomainZero>;

INSTANTIATE_TYPED_TEST_SUITE_P(PrimitiveArrayDomain,
                               AbstractDomainPropertyTest,
                               PrimitiveArrayDomain);

template <>
std::vector<PrimitiveArrayDomain>
AbstractDomainPropertyTest<PrimitiveArrayDomain>::non_extremal_values() {
  PrimitiveArrayDomain empty(0);
  PrimitiveArrayDomain length_one(1);
  PrimitiveArrayDomain length_two(2);
  length_one.set(0, SignedConstantDomain(10));
  length_two.set(0, SignedConstantDomain(10));
  length_two.set(1, SignedConstantDomain(11));
  return {empty, length_one, length_two};
}

TEST_F(ConstantPropagationTest, ConstantArrayOperations) {
  {
    // Top cannot be changed to another value by setting an array index
    PrimitiveArrayDomain arr;
    EXPECT_TRUE(arr.is_top());
    arr.set(0, SignedConstantDomain(1));
    EXPECT_TRUE(arr.is_top());
  }

  {
    // Arrays are zero-initialized
    PrimitiveArrayDomain arr(10);
    EXPECT_EQ(arr.length(), 10);
    for (uint32_t i = 0; i < arr.length(); ++i) {
      EXPECT_EQ(arr.get(i), SignedConstantDomain(0));
    }
    // Check that iterating over the bindings works too
    size_t count = 0;
    for (const auto& pair : arr.bindings()) {
      EXPECT_EQ(pair.second, SignedConstantDomain(0));
      ++count;
    }
    EXPECT_EQ(count, 10);
  }

  {
    // OOB read/write
    for (uint32_t i = 0; i < 10; ++i) {
      PrimitiveArrayDomain arr(i);
      EXPECT_EQ(arr.length(), i);
      EXPECT_TRUE(arr.get(i).is_bottom());
      arr.set(i, SignedConstantDomain(1));
      EXPECT_TRUE(arr.is_bottom());
      EXPECT_THROW(arr.length(), RedexException);
    }
  }

  {
    // join/meet of differently-sized arrays is Top/Bottom respectively
    PrimitiveArrayDomain arr1(1);
    PrimitiveArrayDomain arr2(2);
    EXPECT_TRUE(arr1.join(arr2).is_top());
    EXPECT_TRUE(arr1.meet(arr2).is_bottom());
  }
}

using ArrayAnalyzer = InstructionAnalyzerCombiner<cp::LocalArrayAnalyzer,
                                                  cp::HeapEscapeAnalyzer,
                                                  cp::PrimitiveAnalyzer>;

/*
 * This is a macro instead of a function so that the error messages will contain
 * the right line numbers.
 */
#define VERIFY_NO_CHANGE(code_str)                                   \
  do {                                                               \
    auto _code = assembler::ircode_from_string((code_str));          \
    do_const_prop(_code.get(), ArrayAnalyzer());                     \
    auto _expected_code = assembler::ircode_from_string((code_str)); \
    EXPECT_CODE_EQ(_code.get(), _expected_code.get());               \
  } while (0);

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

     (return-void)
    )
)");

  do_const_prop(code.get(), ArrayAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I")
     (move-result-pseudo-object v2)
     (aput v1 v2 v0)
     (const v3 1)

     (const v0 2)

     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, PrimitiveFillArrayData) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (fill-array-data v2 #4 (1))

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");

  do_const_prop(code.get(), ArrayAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (fill-array-data v2 #4 (1))
     (const v3 1)
     (const v0 2)
     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());

  auto more_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (fill-array-data v2 #4 (ffffff9c)) ; negative 100 in 2's complement

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-ltz v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");

  do_const_prop(more_code.get(), ArrayAnalyzer());

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (fill-array-data v2 #4 (ffffff9c)) ; negative 100 in 2's complement
     (const v3 -100)
     (const v0 2)
     (return-void)
    )
)");
  EXPECT_CODE_EQ(more_code.get(), expected_code2.get());
}

TEST_F(ConstantPropagationTest, PrimitiveFillArrayDataUnknownLen) {
  const auto* code_unknown_len = R"(
    (
     (const v0 0)

     (invoke-static () "LFoo;.bar:()I")
     (move-result v1)

     (new-array v1 "[I") ; create an array of unknown length
     (move-result-pseudo-object v2)
     (fill-array-data v2 #4 (1))

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)";
  VERIFY_NO_CHANGE(code_unknown_len);

  const auto* code_bad_len = R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (fill-array-data v2 #4 (1 2)) ; this should be invalid, cannot fill two items into array of len 1

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)";
  VERIFY_NO_CHANGE(code_bad_len);

  const auto* code_no_idea = R"(
    (
     (const v0 0)

     (invoke-static () "LFoo;.bar:()[I")
     (move-result-object v2)
     (fill-array-data v2 #4 (1))

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)";
  VERIFY_NO_CHANGE(code_no_idea);

  const auto* code_unknown_idx = R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (fill-array-data v2 #4 (1))

     (invoke-static () "LFoo;.bar:()I")
     (move-result v4)
     (aput v4 v2 v4) ; put some value at an unknown index

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)";
  VERIFY_NO_CHANGE(code_unknown_idx);
}

TEST_F(ConstantPropagationTest, PrimitiveFilledNewArray) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (filled-new-array (v1) "[I") ; create an array of length 1
     (move-result-pseudo-object v2)

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");

  do_const_prop(code.get(), ArrayAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (filled-new-array (v1) "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (const v3 1)
     (const v0 2)
     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());

  const auto* code_unknown_val = R"(
    (
     (const v0 0)
     (invoke-static () "LFoo;.bar:()I")
     (move-result v1)
     (filled-new-array (v1) "[I") ; create an array of length 1, with no idea what the contents are
     (move-result-pseudo-object v2)

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)";
  VERIFY_NO_CHANGE(code_unknown_val);
}

TEST_F(ConstantPropagationTest, UnknownPrimitiveArray) {
  // Analyze some instructions filling/getting/putting an array for which its
  // size is unknown. Analyzer should not crash and demonstrate that it knows
  // approximately nothing about the array. Knowing that length is nonnegative
  // is fine but that is not necessarily implemented right now.
  const auto* code_size_unknown = R"(
    (
     (const v0 1)
     (const v1 99)

     (invoke-static () "LFoo;.bar:()[I")
     (move-result-object v2)
     (fill-array-data v2 #4 (1))

     (aput v1 v2 v0) ; write 99 into arr[1] - size of v2 should be unknown so it is unknown if this will throw
     (aget v2 v0) ; this statement may not be reachable, who knows
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v4 1)

     (:if-true-label)
     (const v4 2)

     (return-void)
    )
)";
  VERIFY_NO_CHANGE(code_size_unknown);
}

TEST_F(ConstantPropagationTest, UnknownReturnValuesClearedOut) {
  // Makes sure handling of RESULT_REGISTER is not forgotten when it needs to be
  // reset.
  const auto* code = R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)

     (invoke-static () "LFoo;.bar:()[I")
     (move-result-object v2)

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v4 1)

     (:if-true-label)
     (const v4 2)

     (return-void)
    )
)";
  VERIFY_NO_CHANGE(code);
}

TEST_F(ConstantPropagationTest, ObjectArrayReturnValueClearedOut) {
  // Makes sure handling of RESULT_REGISTER is not forgotten when it needs to be
  // reset for creation of object arrays (which are not being modeled here).
  const auto* code = R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (aput v1 v2 v0)

     ; create an array of strings, first item is null
     (filled-new-array (v0) "[Ljava/lang/String;")
     (move-result-object v2)

     (aget v2 v0)
     (move-result-pseudo v3)

     (if-nez v3 :if-true-label)
     (const v4 1)

     (:if-true-label)
     (const v4 2)

     (return-void)
    )
)";
  VERIFY_NO_CHANGE(code);
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

     (return-void)
    )
)");

  auto expected = assembler::to_s_expr(code.get());
  do_const_prop(code.get(), ArrayAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I")
     (move-result-pseudo-object v2)
     (move-object v3 v2)
     (aput v1 v3 v0)
     (const v4 1)

     (const v0 2)

     (return-void)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, PrimitiveArrayEscapesViaCall) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (const v4 4)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (aput v1 v2 v0) ; write 1 into arr[0]
     (invoke-static (v4) "LFoo;.bar:(I)V")
     (invoke-static (v2) "LFoo;.bar:([I)V") ; bar() might modify the array
     (aget v2 v0)
     (move-result-pseudo v3)

     (if-eqz v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");

  auto expected = assembler::to_s_expr(code.get());
  do_const_prop(code.get(), ArrayAnalyzer());
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

     (return-void)
    )
)");

  auto expected = assembler::to_s_expr(code.get());
  do_const_prop(code.get(), ArrayAnalyzer());
  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}

TEST_F(ConstantPropagationTest, PrimitiveArrayEscapesViaFilledNewArray) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (new-array v1 "[I") ; create an array of length 1
     (move-result-pseudo-object v2)
     (aput v1 v2 v0) ; write 1 into arr[0]
     (move-object v3 v2) ; create an alias
     (filled-new-array (v3) "[[I")
     (move-result-object v4)
     (aget v2 v0)
     (move-result-pseudo v3)

     (if-eqz v3 :if-true-label)
     (const v0 1)

     (:if-true-label)
     (const v0 2)

     (return-void)
    )
)");

  auto expected = assembler::to_s_expr(code.get());
  do_const_prop(code.get(), ArrayAnalyzer());
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
  cp::intraprocedural::FixpointIterator intra_cp(
      /* cp_state */ nullptr, cfg, ArrayAnalyzer());
  intra_cp.run(ConstantEnvironment());
  EXPECT_TRUE(intra_cp.get_exit_state_at(cfg.exit_block()).is_bottom());
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
  cp::intraprocedural::FixpointIterator intra_cp(
      /* cp_state */ nullptr, cfg, ArrayAnalyzer());
  intra_cp.run(ConstantEnvironment());
  EXPECT_TRUE(intra_cp.get_exit_state_at(cfg.exit_block()).is_bottom());
}
