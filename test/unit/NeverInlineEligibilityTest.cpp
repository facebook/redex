/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>

#include <gtest/gtest.h>

#include "NeverInlineEligibility.h"

#include "Creators.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "Show.h"

// Truth-table tests for the shape-eligibility decision itself. Callers test
// their own use of it.
namespace {

class NeverInlineEligibilityTest : public RedexTest {
 public:
  static DexMethod* make_method(const std::string& sig,
                                const std::string& code_str) {
    static std::atomic<size_t> counter{0};
    size_t c = counter.fetch_add(1);
    std::string name = std::string("LFoo") + std::to_string(c) + ";";
    ClassCreator cc{DexType::make_type(name)};
    cc.set_super(type::java_lang_Object());
    auto* m =
        DexMethod::make_method(name + ".bar:" + sig)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                            assembler::ircode_from_string(code_str), false);
    m->set_deobfuscated_name(show(m));
    m->get_code()->build_cfg();
    cc.add_method(m);
    cc.create();
    return m;
  }
};

} // namespace

TEST_F(NeverInlineEligibilityTest, EligibleOrdinaryMethod) {
  auto* m = make_method("(I)I", R"((
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (add-int v0 v0 v1)
      (add-int v0 v0 v2)
      (return v0)
    ))");
  EXPECT_EQ(
      never_inline_analysis::never_inline_eligibility(
          m, /* max_callee_code_units */ 40, /* min_callee_instructions */ 4),
      never_inline_analysis::Ineligibility::kEligible);
}

TEST_F(NeverInlineEligibilityTest, AlwaysThrowsHasNoReturnBlock) {
  auto* m = make_method("(I)V", R"((
      (load-param v0)
      (new-instance "Ljava/lang/Exception;")
      (move-result-pseudo-object v1)
      (throw v1)
    ))");
  EXPECT_EQ(
      never_inline_analysis::never_inline_eligibility(
          m, /* max_callee_code_units */ 40, /* min_callee_instructions */ 1),
      never_inline_analysis::Ineligibility::kAlwaysThrows);
}

TEST_F(NeverInlineEligibilityTest, TooLargeOverCodeUnitCeiling) {
  auto* m = make_method("(I)I", R"((
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (add-int v0 v0 v1)
      (add-int v0 v0 v2)
      (return v0)
    ))");
  EXPECT_EQ(
      never_inline_analysis::never_inline_eligibility(
          m, /* max_callee_code_units */ 1, /* min_callee_instructions */ 1),
      never_inline_analysis::Ineligibility::kTooLarge);
}

TEST_F(NeverInlineEligibilityTest, TooSmallUnderInstructionFloor) {
  auto* m = make_method("(I)I", R"((
      (load-param v0)
      (const v1 1)
      (add-int v0 v0 v1)
      (return v0)
    ))");
  EXPECT_EQ(never_inline_analysis::never_inline_eligibility(
                m, /* max_callee_code_units */ 40,
                /* min_callee_instructions */ 100),
            never_inline_analysis::Ineligibility::kTooSmall);
}

TEST_F(NeverInlineEligibilityTest, SimpleForwarderIsIneligible) {
  auto* m = make_method("(I)I", R"((
      (load-param v0)
      (return v0)
    ))");
  EXPECT_EQ(
      never_inline_analysis::never_inline_eligibility(
          m, /* max_callee_code_units */ 40, /* min_callee_instructions */ 1),
      never_inline_analysis::Ineligibility::kSimple);
}

TEST_F(NeverInlineEligibilityTest, IsSimpleTrueForTrivialForwarder) {
  auto* m = make_method("(I)I", R"((
      (load-param v0)
      (return v0)
    ))");
  EXPECT_TRUE(never_inline_analysis::is_simple(m));
}

TEST_F(NeverInlineEligibilityTest, IsSimpleFalseForOrdinaryMethod) {
  auto* m = make_method("(I)I", R"((
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (add-int v0 v0 v1)
      (add-int v0 v0 v2)
      (return v0)
    ))");
  EXPECT_FALSE(never_inline_analysis::is_simple(m));
}

TEST_F(NeverInlineEligibilityTest, IsSimplePopulatesInvokeInsnForForwarder) {
  static std::atomic<size_t> counter{0};
  size_t c = counter.fetch_add(1);
  std::string name = std::string("LBaz") + std::to_string(c) + ";";
  ClassCreator cc{DexType::make_type(name)};
  cc.set_super(type::java_lang_Object());
  auto* callee = DexMethod::make_method(name + ".callee:()I")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                     assembler::ircode_from_string(R"((
                                        (const v0 1)
                                        (return v0)
                                      ))"),
                                     false);
  callee->set_deobfuscated_name(show(callee));
  callee->get_code()->build_cfg();
  cc.add_method(callee);
  auto* caller = DexMethod::make_method(name + ".caller:()I")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                     assembler::ircode_from_string(
                                         "(\n"
                                         "  (invoke-static () \"" +
                                         name +
                                         ".callee:()I\")\n"
                                         "  (move-result v0)\n"
                                         "  (return v0)\n"
                                         ")"),
                                     false);
  caller->set_deobfuscated_name(show(caller));
  caller->get_code()->build_cfg();
  cc.add_method(caller);
  cc.create();

  IRInstruction* invoke_insn = nullptr;
  EXPECT_TRUE(never_inline_analysis::is_simple(caller, &invoke_insn));
  ASSERT_NE(invoke_insn, nullptr);
  EXPECT_TRUE(opcode::is_an_invoke(invoke_insn->opcode()));
}
