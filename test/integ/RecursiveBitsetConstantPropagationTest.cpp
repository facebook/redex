/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexUtil.h"
#include "IPConstantPropagation.h"
#include "RedexTest.h"
#include "Resolver.h"
#include "Show.h"

namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;

class RecursiveBitsetConstantPropagationTest
    : public RedexIntegrationTest,
      public ::testing::WithParamInterface<std::string> {
 private:
  void set_root_method(std::string_view full_name) {
    auto* method = DexMethod::get_method(full_name)->as_def();
    ASSERT_THAT(method, NotNull()) << "method " << full_name << " not found";
    method->rstate.set_root();
  }

 protected:
  void SetUp() override {
    static constexpr std::string_view main_method_sig =
        "LRecursiveBitsetConstantPropagation;.main:()V";
    RedexIntegrationTest::SetUp();
    set_root_method(main_method_sig);
    auto* const main_method = DexMethod::get_method(main_method_sig)->as_def();
    auto* code_main = main_method->get_code();
    ASSERT_THAT(code_main, NotNull()) << "main method not found";
  }

  const std::string& process_method_sig = GetParam();
  static constexpr auto lowest_bit_set = "Lowest bit is set";
  static constexpr auto second_lowest_bit_set = "Second lowest bit is set";
};

TEST_P(RecursiveBitsetConstantPropagationTest,
       BeforeOptimizationAllBranchesArePresent) {
  auto* const process_method = DexMethod::get_method(process_method_sig);
  ASSERT_TRUE(process_method->is_def()) << "process method not defined";
  auto* const code_process_method = process_method->as_def()->get_code();
  ASSERT_THAT(code_process_method, NotNull()) << "process method not found";
  EXPECT_THAT(assembler::to_string(code_process_method),
              HasSubstr(lowest_bit_set))
      << "The method does not have a block for lowest bit being set";
  EXPECT_THAT(assembler::to_string(code_process_method),
              HasSubstr(second_lowest_bit_set))
      << "The method does not have a block for second lowest bit being set";
}

TEST_P(RecursiveBitsetConstantPropagationTest,
       AfterOptimizationOnlySecondLowestBitIsGone) {
  Pass* constp = new InterproceduralConstantPropagationPass();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  auto* const process_method = DexMethod::get_method(process_method_sig);
  ASSERT_TRUE(process_method->is_def()) << "process method not defined";
  auto* const code_process_method = process_method->as_def()->get_code();
  ASSERT_THAT(code_process_method, NotNull()) << "process method not found";
  EXPECT_THAT(assembler::to_string(code_process_method),
              HasSubstr(lowest_bit_set))
      << "Lowest bit is set, but the method does not have a block for it";
  EXPECT_THAT(assembler::to_string(code_process_method),
              Not(HasSubstr(second_lowest_bit_set)))
      << "Second lowest bit is not set, but the method has a block for it";
}

INSTANTIATE_TEST_SUITE_P(
    RecursiveBitsetConstantPropagationTests,
    RecursiveBitsetConstantPropagationTest,
    ::testing::Values(
        "LRecursiveBitsetConstantPropagation;.processWithLambda:(II)V",
        "LRecursiveBitsetConstantPropagation;.processWithNoLambda:(II)V"));

} // namespace
