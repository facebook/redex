/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

TEST_F(ConstantPropagationTest, ArrayLengthNonNegative) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (array-length v0)
      (move-result-pseudo v0)
      (if-ltz v0 :next)
      (:next)
      (return-void)
    )
  )");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (array-length v0)
      (move-result-pseudo v0)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(ConstantPropagationTest, DereferenceWithoutThrowBlock) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (array-length v0)
      (move-result-pseudo v1)
      (if-eqz v0 :next)
      (:next)
      (return-void)
    )
  )");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (array-length v0)
      (move-result-pseudo v1)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(ConstantPropagationTest, DereferenceWithThrowBlock) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (.try_start a)
      (array-length v0)
      (move-result-pseudo v1)
      (.try_end a)
      (if-eqz v0 :next1)
      (:next1)
      (return-void)
      (.catch (a))
      (if-eqz v0 :next2)
      (:next2)
      (return-void)
    )
  )");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (.try_start a)
      (array-length v0)
      (move-result-pseudo v1)
      (.try_end a)
      (return-void)
      (.catch (a))
      (if-eqz v0 :next2)
      (:next2)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(ConstantPropagationTest, NullCheckCastYieldsNull) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (check-cast v0 "LFoo;")
     (move-result-pseudo v1)
     (if-eqz v1 :next)
     (const v2 1)
     (goto :end)
     (:next)
     (const v2 2)
     (:end)
     (return-void)
    )
  )");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (goto :next)
      (const v2 1)
      (goto :end)
      (:next)
      (const v2 2)
      (:end)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, JumpToImmediateNext) {
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
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, InstanceOfNull) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (instance-of v0 "Ljava/lang/String;")
     (move-result-pseudo v1)
     (if-eqz v1 :next)
     (const v2 1)
     (goto :end)
     (:next)
     (const v2 2)
     (:end)
     (return-void)
    )
  )");

  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (goto :next)
      (const v2 1)
      (goto :end)
      (:next)
      (const v2 2)
      (:end)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

// A typical case where a non-default block is uniquely reachable.
TEST_F(ConstantPropagationTest, Switch1) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (switch v0 (:b :c))
     (const v1 100)
     (return v1)

     (:b 1) ; reachable
     (const v1 200)
     (return v1)

     (:c 3) ; unreachable
     (const v1 300)
     (return v1)
  )

  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (goto :b)

     (const v1 100)
     (return v1)

     (:b) ; reachable
     (const v1 200)
     (return v1)

     (const v1 300)
     (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// Default block also has a unreachable label.
TEST_F(ConstantPropagationTest, Switch2) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (switch v0 (:a :b :c))

     (:a 0) ; default or unreachable
     (const v1 100)
     (return v1)

     (:b 1) ; reachable
     (const v1 200)
     (return v1)

     (:c 3) ; unreachable
     (const v1 300)
     (return v1)
  )

  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (goto :b)

     (const v1 100)
     (return v1)

     (:b) ; reachable
     (const v1 200)
     (return v1)

     (const v1 300)
     (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// Multiple unreachables labels fall into a block
TEST_F(ConstantPropagationTest, Switch3) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (switch v0 (:b :c :d))

     (const v1 100)
     (return v1)

     (:b 1) ; reachable
     (const v1 200)
     (return v1)

     (:c 3) ; unreachable
     (:d 4) ; unreachable
     (const v1 300)
     (return v1)
    )
  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (goto :b)

     (const v1 100)
     (return v1)

     (:b) ; reachable
     (const v1 200)
     (return v1)

     (const v1 300)
     (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// When reachable and unreachable fall into a same block
TEST_F(ConstantPropagationTest, Switch4) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (switch v0 (:b :c :d))

     (const v1 100)
     (return v1)

     (:b 1) ; reachable
     (:c 3) ; unreachable
     (const v1 200)
     (return v1)

     (:d 4) ; unreachable
     (const v1 300)
     (return v1)
    )
  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (goto :b)

     (const v1 100)
     (return v1)

     (:b) ; reachable
     (const v1 200)
     (return v1)

     (const v1 300)
     (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// Except default block, all are unreachable
// Switch is just deleted.
TEST_F(ConstantPropagationTest, Switch5) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 3)
     (switch v0 (:b :d))

     (const v1 100)
     (return v1)

     (:b 1) ; unreachable
     (const v1 200)
     (return v1)

     (:d 4) ; unreachable
     (const v1 300)
     (return v1)
    )
  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 3)

     (const v1 100)
     (return v1)

     (const v1 200)
     (return v1)

     (const v1 300)
     (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// Except default block with a switch target, all are unreachable.
// Switch is just deleted.
TEST_F(ConstantPropagationTest, Switch6) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 2)
     (switch v0 (:a :b :d))

     (:a 2)
     (const v1 100)
     (return v1)

     (:b 1) ; unreachable
     (const v1 200)
     (return v1)

     (:d 4) ; unreachable
     (const v1 300)
     (return v1)
    )
  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 2)

     (const v1 100)
     (return v1)

     (const v1 200)
     (return v1)

     (const v1 300)
     (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// A uniquely non-default case with constant.
TEST_F(ConstantPropagationTest, SwitchOnExactConstant) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (switch v0 (:b))
      ; unreachable
      (const v1 100)
      (return v1)

      (:b 1) ; reachable
      (const v1 200)
      (return v1)
    )
  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (goto :b)
      ; unreachable
      (const v1 100)
      (return v1)

      (:b) ; reachable
      (const v1 200)
      (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(ConstantPropagationTest, SwitchOnInterval) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-gez v0 :a)
      (const v0 0)
      (:a)
      ; at this point, we know v0 is >= 0

      (switch v0 (:b))
      ; reachable
      (const v1 100)
      (return v1)
      (:b 1) ; reachable
      (const v1 200)
      (return v1)
    )
  )");

  auto original = assembler::to_s_expr(code.get());
  do_const_prop(code.get());

  EXPECT_EQ(assembler::to_s_expr(code.get()), original) << show(code.get());
}

// A uniquely non-default case with non-constant.
// Do not optimize this since default is reachable.
TEST_F(ConstantPropagationTest, Switch8) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (switch v0 (:b))
      ; reachable
      (const v1 100)
      (return v1)

      (:b 1) ; reachable
      (const v1 200)
      (return v1)
    )
  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (switch v0 (:b))
      ; reachable
      (const v1 100)
      (return v1)

      (:b 1) ; reachable
      (const v1 200)
      (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// Remove dead switch if no non-default block exists.
TEST_F(ConstantPropagationTest, Switch9) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (switch v0  (:a :b))
      (:b 1) ; reachable
      (:a 2) ;
      (const v1 200)
      (return v1)
    )
  )");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (const v1 200)
      (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(ConstantPropagationTest, WhiteBox1) {
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

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  cp::intraprocedural::FixpointIterator intra_cp(
      cfg, cp::ConstantPrimitiveAnalyzer());
  intra_cp.run(ConstantEnvironment());

  auto exit_state = intra_cp.get_exit_state_at(cfg.exit_block());
  // Specifying `0u` here to avoid any ambiguity as to whether it is the null
  // pointer
  EXPECT_EQ(exit_state.get<SignedConstantDomain>(0u),
            SignedConstantDomain::top());
  EXPECT_EQ(exit_state.get<SignedConstantDomain>(1), SignedConstantDomain(0));
  // v2 can contain either the value 0 or 1
  EXPECT_EQ(exit_state.get<SignedConstantDomain>(2),
            SignedConstantDomain(sign_domain::Interval::GEZ));
  EXPECT_EQ(exit_state.get<SignedConstantDomain>(3), SignedConstantDomain(0));
}

TEST_F(ConstantPropagationTest, WhiteBox2) {
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

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  cp::intraprocedural::FixpointIterator intra_cp(
      cfg, cp::ConstantPrimitiveAnalyzer());
  intra_cp.run(ConstantEnvironment());

  auto exit_state = intra_cp.get_exit_state_at(cfg.exit_block());
  EXPECT_EQ(exit_state.get<SignedConstantDomain>(0u),
            SignedConstantDomain(sign_domain::Interval::GEZ));
  EXPECT_EQ(exit_state.get<SignedConstantDomain>(1), SignedConstantDomain(0));
}

TEST_F(ConstantPropagationTest, ForwardBranchesIf) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :L1)
      (const v0 1)
      (goto :L2)
      (:L1)
      (const v0 0)
      (:L2)
      (if-eqz v0 :L3)
      (:L4)
      (const v0 0)
      (:L3)
      (return-void)
    )
  )");

  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config(),
                /* editable_cfg */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :L1)
      (:L1)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ForwardBranchesIfSideEffectFreeComputation) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :L1)
      (const v0 1)
      (goto :L2)
      (:L1)
      (const v0 42)
      (sub-int v0 v0 v0)
      (:L2)
      (if-eqz v0 :L3)
      (:L4)
      (const v0 0)
      (:L3)
      (return-void)
    )
  )");

  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config(),
                /* editable_cfg */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :L1)
      (:L1)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ForwardBranchesIfSideEffectingComputation) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :L1)
      (const v0 1)
      (goto :L2)
      (:L1)
      (const v0 42)
      (div-int v0 v0)
      (move-result-pseudo v0) ; thiss instruction isn't supported yet
      (:L2)
      (if-eqz v0 :L3)
      (:L4)
      (const v0 0)
      (:L3)
      (return-void)
    )
  )");

  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config(),
                /* editable_cfg */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :L1)
      (:L3)
      (return-void)
      (:L1)
      (const v0 42)
      (div-int v0 v0)
      (move-result-pseudo v0) ; thiss instruction isn't supported yet
      (goto :L3)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, ForwardBranchesSwitch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (if-eqz v0 :L0)
      (if-eqz v1 :L1)
      (const v0 2)
      (goto :SWITCH)
      (:L0)
      (const v0 0)
      (goto :SWITCH)
      (:L1)
      (const v0 1)
      (goto :SWITCH)

      (:SWITCH)
      (switch v0 (:S0 :S1))
      (:FALLTHROUGH)
      (const v0 2)
      (goto :END)
      (:S0 0)
      (const v0 0)
      (goto :END)
      (:S1 1)
      (const v0 1)
      (goto :END)
      (:END)
      (return-void)
    )
  )");

  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config(),
                /* editable_cfg */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (if-eqz v0 :L0)
      (if-eqz v1 :L1)
      (:L0)
      (:L1)
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, RedundantNullCheck) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/Object;I)V")
      (invoke-static (v1) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/Object;I)V")
      (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/Object;I)V")
      (return-void)
    )
  )");

  DexMethod::make_method(
      kotlin_nullcheck_wrapper::NEW_CHECK_EXPR_NULL_SIGNATURE_V1_4);
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config(),
                /* editable_cfg */ false);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/Object;I)V")
      (invoke-static (v1) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/Object;I)V")
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, RedundantNullCheckCmp) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/Object;I)V")
      (if-eqz v0 :L0)
      (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/Object;I)V")
      (:L0)
      (return-void)
    )
  )");

  DexMethod::make_method(
      kotlin_nullcheck_wrapper::NEW_CHECK_EXPR_NULL_SIGNATURE_V1_4);
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config(),
                /* editable_cfg */ false);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_4:(Ljava/lang/Object;I)V")
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
