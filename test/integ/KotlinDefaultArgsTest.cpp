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
#include "RedexTest.h"
#include "Resolver.h"
#include "Show.h"

namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NotNull;

class KotlinDefaultArgsTest : public RedexIntegrationTest {
 private:
  void set_root_method(std::string_view full_name) {
    auto method = DexMethod::get_method(full_name)->as_def();
    ASSERT_THAT(method, NotNull()) << "method " << full_name << " not found";
    method->rstate.set_root();
  }

 protected:
  void SetUp() override {
    static constexpr std::string_view main_method_sig =
        "LKotlinDefaultArgs;.main:()V";
    RedexIntegrationTest::SetUp();

    set_root_method(main_method_sig);
    const auto main_method = DexMethod::get_method(main_method_sig)->as_def();
    auto code_main = main_method->get_code();
    ASSERT_THAT(code_main, NotNull()) << "main method not found";
  }

  static constexpr std::string_view greet_method_signature =
      "LKotlinDefaultArgs;.greet:(Ljava/lang/String;Ljava/lang/String;)V";
  static constexpr std::string_view greet_default_method_signature =
      "LKotlinDefaultArgs;.greet$default:(LKotlinDefaultArgs;Ljava/lang/"
      "String;Ljava/lang/String;ILjava/lang/Object;)V";
};

TEST_F(KotlinDefaultArgsTest, UnoptimizedGreetHasHelloAnd) {
  // Sanity check on unoptimized code.

  const auto default_arg_method = DexMethod::get_method(greet_method_signature);
  auto code_default_arg = default_arg_method->as_def()->get_code();
  ASSERT_THAT(code_default_arg, NotNull());

  const auto syn_default_arg_method =
      DexMethod::get_method(greet_default_method_signature);
  auto code_syn_default_arg = syn_default_arg_method->as_def()->get_code();
  ASSERT_THAT(code_syn_default_arg, NotNull())
      << "Synthetic default method not found";
  EXPECT_THAT(assembler::to_string(code_syn_default_arg), HasSubstr("Hello"))
      << "\"Hello\" is the default value of the second arg \"greeting\", but "
         "is missing before optimization";
  EXPECT_THAT(assembler::to_string(code_syn_default_arg), HasSubstr("and-"))
      << "The synthetic default method does not contain \"and-*\" instructions "
         "before optimization";
}

TEST_F(KotlinDefaultArgsTest, OptimizedGreetDoesNotHaveHelloAnd) {
  const auto default_arg_method = DexMethod::get_method(greet_method_signature);
  auto code_default_arg = default_arg_method->as_def()->get_code();
  ASSERT_THAT(code_default_arg, NotNull());

  Pass* constp = new constant_propagation::interprocedural::PassImpl();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  const auto syn_default_arg_method =
      DexMethod::get_method(greet_default_method_signature);
  auto code_syn_default_arg = syn_default_arg_method->as_def()->get_code();
  ASSERT_THAT(code_syn_default_arg, NotNull())
      << "Synthetic default method not found";
  EXPECT_THAT(assembler::to_string(code_syn_default_arg), HasSubstr("Guest"))
      << "Default arg \"name\" is used, but the synthetic default method has "
         "dropped its default value \"Guest\"";
  EXPECT_THAT(assembler::to_string(code_syn_default_arg),
              Not(HasSubstr("Hello")))
      << "Default arg \"greeting\" is never used, but the synthetic default "
         "method still contains its default value \"Hello\"";
  EXPECT_THAT(assembler::to_string(code_syn_default_arg),
              Not(HasSubstr("and-")))
      << "Only one default arg is used, but the synthetic default method still "
         "contains \"and-*\" instructions";
}
} // namespace
