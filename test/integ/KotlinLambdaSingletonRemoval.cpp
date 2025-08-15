/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexUtil.h"
#include "KotlinStatelessLambdaSingletonRemovalPass.h"
#include "RedexTest.h"
#include "Resolver.h"
#include "Show.h"

namespace {
using ::testing::NotNull;

class KotlinLambdaOptTest : public RedexIntegrationTest,
                            public ::testing::WithParamInterface<std::string> {
 protected:
  void set_root_method(std::string_view full_name) {
    auto method = DexMethod::get_method(full_name)->as_def();
    ASSERT_NE(nullptr, method);
    method->rstate.set_root();
  }

  void check_sget_available(IRCode* code) {
    std::cerr << "BEFORE = " << SHOW(code) << std::endl;
    auto ii = InstructionIterable(code);
    auto end = ii.end();
    fprintf(stderr, "%s\n", SHOW(code));
    bool found = false;
    for (auto it = ii.begin(); it != end; ++it) {
      auto insn = it->insn;
      if (insn->opcode() == OPCODE_SGET_OBJECT) {
        found = true;
      }
    }
    EXPECT_TRUE(found) << "SGET not found in " << SHOW(code);
  }
  void check_sget_not_available(IRCode* code) {
    std::cerr << "AFTER = " << SHOW(code) << std::endl;
    auto ii = InstructionIterable(code);
    auto end = ii.end();
    for (auto it = ii.begin(); it != end; ++it) {
      auto insn = it->insn;
      EXPECT_NE(insn->opcode(), OPCODE_SGET_OBJECT)
          << "SGET found in " << SHOW(code);
    }
  }
};

TEST_F(KotlinLambdaOptTest, LambdaSingletonIsRemoved) {
  auto scope = build_class_scope(stores);
  constexpr std::string_view root_method_name =
      "LKotlinLambdaSingletonRemoval;.foo:()V";
  set_root_method(root_method_name);

  auto* lambda_class =
      type_class(DexType::make_type("LKotlinLambdaSingletonRemoval$foo$1;"));
  ASSERT_THAT(lambda_class, NotNull());
  lambda_class->set_deobfuscated_name("LKotlinLambdaSingletonRemoval$foo$1;");

  auto x_method = DexMethod::get_method(root_method_name)->as_def();
  auto codex = x_method->get_code();
  ASSERT_THAT(codex, NotNull());
  check_sget_available(codex);

  auto klr = new KotlinStatelessLambdaSingletonRemovalPass();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  check_sget_not_available(codex);
}

// TODO(T144851518): This test does nothing meaningful because the deobfuscated
// name of the otherwise Lambda class is always empty. Update the test to do
// something meaningful.
TEST_P(KotlinLambdaOptTest, NoEffectOnNonLambda) {
  auto scope = build_class_scope(stores);
  const auto& class_name = GetParam();
  const std::string root_method = class_name + ";.bar:()V";

  set_root_method(root_method);
  auto y_method = DexMethod::get_method(root_method)->as_def();
  auto codey = y_method->get_code();
  ASSERT_THAT(codey, NotNull());
  check_sget_available(codey);

  auto klr = new KotlinStatelessLambdaSingletonRemovalPass();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  check_sget_available(codey);
}

INSTANTIATE_TEST_SUITE_P(
    KotlinLambdaOptTests,
    KotlinLambdaOptTest,
    ::testing::Values("LKotlinInstanceRemovalNamedEquiv",
                      "LKotlinInstanceRemovalEquivNegative",
                      "LKotlinInstanceRemovalEquivNegative2",
                      "LKotlinInstanceRemovalEquivNegative3"));

} // namespace
