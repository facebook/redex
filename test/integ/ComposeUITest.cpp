/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexUtil.h"
#include "IPConstantPropagation.h"
#include "IRList.h"
#include "RedexTest.h"
#include "Resolver.h"

namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;

static constexpr std::string_view default_method_name = "getTestDefault";

bool containsGetTestDefaultCall(const IRCode& code) {
  for (const auto& mie : InstructionIterable(&code)) {
    if (mie.insn->opcode() == OPCODE_INVOKE_STATIC &&
        mie.insn->get_method()->get_name()->str() == default_method_name) {
      return true;
    }
  }
  return false;
}

MATCHER(ContainsGetTestDefaultCall, "Contains getTestDefault call") {
  return containsGetTestDefaultCall(arg);
}

class ComposeUITest : public RedexIntegrationTest {
 private:
  static void set_root_method(std::string_view full_name) {
    auto* method = DexMethod::get_method(full_name)->as_def();
    ASSERT_THAT(method, NotNull()) << "method " << full_name << " not found";
    method->rstate.set_root();
  }

 protected:
  void SetUp() override {
    static constexpr std::string_view main_method_sig =
        "Lredex/ComposeUITestKt;.HelloWorldText:(Landroidx/compose/runtime/"
        "Composer;I)V";
    RedexIntegrationTest::SetUp();

    set_root_method(main_method_sig);
    auto* const main_method = DexMethod::get_method(main_method_sig)->as_def();
    auto* code_main = main_method->get_code();
    ASSERT_THAT(code_main, NotNull()) << "HelloWorldText method not found";
  }

  static constexpr std::string_view super_text_printer_method_signature =
      "Lredex/ComposeUITestKt;.SuperTextPrinter:(Ljava/lang/String;Ljava/lang/"
      "String;Landroidx/compose/runtime/Composer;II)V";
};

TEST_F(ComposeUITest, UnoptimizedHasTestDefault) {
  // Sanity check that ensures input isn't already optimized.
  auto* const super_text_method =
      DexMethod::get_method(super_text_printer_method_signature);
  auto* const code_super_text = super_text_method->as_def()->get_code();
  ASSERT_THAT(code_super_text, NotNull()) << "SuperText method not found";
  EXPECT_THAT(*code_super_text, ContainsGetTestDefaultCall())
      << default_method_name << " call is unexpectedly optimized out: "
      << assembler::to_string(code_super_text);
}

TEST_F(ComposeUITest, OptimizedDoesNotHaveTestDefault) {
  Pass* constp = new InterproceduralConstantPropagationPass();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  auto* const super_text_method =
      DexMethod::get_method(super_text_printer_method_signature);
  auto* code_super_text = super_text_method->as_def()->get_code();
  ASSERT_THAT(code_super_text, NotNull()) << "SuperText method not found";
  EXPECT_THAT(*code_super_text, Not(ContainsGetTestDefaultCall()))
      << default_method_name << " call is unexpectedly optimized out: "
      << assembler::to_string(code_super_text);
}
} // namespace
