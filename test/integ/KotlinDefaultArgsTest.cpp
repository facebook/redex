/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexUtil.h"
#include "IPConstantPropagation.h"
#include "RedexTest.h"
#include "Resolver.h"
#include "Show.h"

using ::testing::HasSubstr;
using ::testing::Not;

class KotlinDefaultArgsTest : public RedexIntegrationTest {
 protected:
  void set_root_method(const std::string& full_name) {
    auto method = DexMethod::get_method(full_name)->as_def();
    ASSERT_NE(nullptr, method);
    method->rstate.set_root();
  }
};
namespace {

TEST_F(KotlinDefaultArgsTest, GreetDoesNotHaveBlock) {
  auto scope = build_class_scope(stores);

  set_root_method("LKotlinDefaultArgs;.main:()V");
  const auto main_method =
      DexMethod::get_method("LKotlinDefaultArgs;.main:()V")->as_def();
  auto code_main = main_method->get_code();
  ASSERT_NE(nullptr, code_main);

  const auto default_arg_method = DexMethod::get_method(
      "LKotlinDefaultArgs;.greet:(Ljava/lang/String;Ljava/lang/String;)V");
  auto code_default_arg = default_arg_method->as_def()->get_code();
  ASSERT_NE(nullptr, code_default_arg);

  Pass* constp = new constant_propagation::interprocedural::PassImpl();
  std::vector<Pass*> passes = {constp};
  run_passes(passes);

  const auto syn_default_arg_method = DexMethod::get_method(
      "LKotlinDefaultArgs;.greet$default:(LKotlinDefaultArgs;Ljava/lang/"
      "String;Ljava/lang/String;ILjava/lang/Object;)V");
  auto code_syn_default_arg = syn_default_arg_method->as_def()->get_code();
  ASSERT_NE(nullptr, code_syn_default_arg);
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
