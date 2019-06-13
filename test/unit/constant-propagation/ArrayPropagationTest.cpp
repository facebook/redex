/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagation.h"

#include <gtest/gtest.h>

#include "AbstractDomainPropertyTest.h"
#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

using PrimitiveArrayDomain = ConstantArrayDomain<SignedConstantDomain>;

INSTANTIATE_TYPED_TEST_CASE_P(PrimitiveArrayDomain,
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
      EXPECT_THROW(arr.length(), RedexException);
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

using ArrayAnalyzer = InstructionAnalyzerCombiner<cp::LocalArrayAnalyzer,
                                                  cp::HeapEscapeAnalyzer,
                                                  cp::PrimitiveAnalyzer>;

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

  do_const_prop(code.get(), ArrayAnalyzer());

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

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  cp::intraprocedural::FixpointIterator intra_cp(cfg, ArrayAnalyzer());
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

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  cp::intraprocedural::FixpointIterator intra_cp(cfg, ArrayAnalyzer());
  intra_cp.run(ConstantEnvironment());
  EXPECT_TRUE(intra_cp.get_exit_state_at(cfg.exit_block()).is_bottom());
}
