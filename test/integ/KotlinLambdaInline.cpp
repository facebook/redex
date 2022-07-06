/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexUtil.h"
#include "RedexTest.h"
#include "Resolver.h"
#include "RewriteKotlinSingletonInstance.h"
#include "Show.h"

class KotlinLambdaOptTest : public RedexIntegrationTest {
 protected:
  void set_root_method(const std::string& full_name) {
    auto method = DexMethod::get_method(full_name)->as_def();
    ASSERT_NE(nullptr, method);
    method->rstate.set_root();
  }
  void check_sget_available(IRCode* code) {
    fprintf(stderr, "BEFORE = %s\n", SHOW(code));
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
    EXPECT_NE(found, false);
  }
  void check_sget_not_available(IRCode* code) {
    fprintf(stderr, "AFTER = %s\n", SHOW(code));
    auto ii = InstructionIterable(code);
    auto end = ii.end();
    for (auto it = ii.begin(); it != end; ++it) {
      auto insn = it->insn;
      EXPECT_NE(insn->opcode(), OPCODE_SGET_OBJECT);
    }
  }
};
namespace {

TEST_F(KotlinLambdaOptTest, MethodHasNoEqDefined) {
  auto scope = build_class_scope(stores);
  set_root_method("LKotlinLambdaInline;.foo:()V");
  auto x_method =
      DexMethod::get_method("LKotlinLambdaInline;.foo:()V")->as_def();
  auto codex = x_method->get_code();
  ASSERT_NE(nullptr, codex);
  check_sget_available(codex);

  set_root_method("LKotlinInstanceRemovalEquiv;.bar:()V");
  auto y_method =
      DexMethod::get_method("LKotlinInstanceRemovalEquiv;.bar:()V")->as_def();
  auto codey = y_method->get_code();
  ASSERT_NE(nullptr, codey);
  check_sget_available(codey);

  set_root_method("LKotlinInstanceRemovalEquivNegative;.bar:()V");
  auto z_method =
      DexMethod::get_method("LKotlinInstanceRemovalEquivNegative;.bar:()V")
          ->as_def();
  auto codez = z_method->get_code();
  ASSERT_NE(nullptr, codez);
  check_sget_available(codez);

  set_root_method("LKotlinInstanceRemovalEquivNegative2;.bar:()V");
  auto l_method =
      DexMethod::get_method("LKotlinInstanceRemovalEquivNegative2;.bar:()V")
          ->as_def();
  auto codel = l_method->get_code();
  ASSERT_NE(nullptr, codel);
  check_sget_available(codel);

  set_root_method("LKotlinInstanceRemovalEquivNegative3;.bar:()V");
  auto m_method =
      DexMethod::get_method("LKotlinInstanceRemovalEquivNegative3;.bar:()V")
          ->as_def();
  auto codem = m_method->get_code();
  ASSERT_NE(nullptr, codem);
  check_sget_available(codem);

  auto klr = new RewriteKotlinSingletonInstance();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  check_sget_not_available(codex);
  check_sget_not_available(codey);
  check_sget_available(codez);
  check_sget_available(codel);
  check_sget_available(codem);
}
} // namespace
