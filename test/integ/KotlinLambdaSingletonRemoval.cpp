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

class KotlinLambdaOptTest : public RedexIntegrationTest {
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
  // Check that instructions for creating a new instance are used.
  void check_removal_result_new_instance(const IRCode* code,
                                         std::string_view lambda_name) {
    const auto ii = InstructionIterable(code);
    const auto new_instance_instruction = std::find_if(
        ii.begin(), ii.end(), [lambda_name](const MethodItemEntry& mie) {
          return mie.insn->opcode() == OPCODE_NEW_INSTANCE &&
                 mie.insn->get_type() == DexType::get_type(lambda_name);
        });
    ASSERT_NE(new_instance_instruction, ii.end())
        << "new-instance not found in " << SHOW(code);

    const auto move_pseudo_result_instruction =
        std::next(new_instance_instruction);
    ASSERT_EQ(move_pseudo_result_instruction->insn->opcode(),
              IOPCODE_MOVE_RESULT_PSEUDO_OBJECT)
        << "move-result-pseudo-object not following new-instance in "
        << SHOW(code);
    const auto result_reg = move_pseudo_result_instruction->insn->dest();

    const auto invoke_direct_instruction =
        std::next(move_pseudo_result_instruction);
    ASSERT_EQ(invoke_direct_instruction->insn->opcode(), OPCODE_INVOKE_DIRECT)
        << "invoke-direct not following move-result-pseudo-object in "
        << SHOW(code);
    ASSERT_EQ(invoke_direct_instruction->insn->srcs_size(), 1u)
        << "Sanity check: Unexpected number of sources for invoke-direct in "
        << SHOW(code);
    ASSERT_EQ(invoke_direct_instruction->insn->src(0), result_reg)
        << "invoke-direct following move-result-pseudo-object does not apply "
           "to the dest register of move-result-pseudo-object in "
        << SHOW(code);
    ASSERT_EQ(invoke_direct_instruction->insn->get_method()->get_class()->str(),
              lambda_name)
        << "invoke-direct following move-result-pseudo-object does not apply "
           "to "
        << lambda_name << " in " << SHOW(code);
    ASSERT_EQ(invoke_direct_instruction->insn->get_method()->get_name()->str(),
              "<init>")
        << "invoke-direct following move-result-pseudo-object does not call "
           "<init> in "
        << SHOW(code);
  }
};

TEST_F(KotlinLambdaOptTest, LambdaSingletonIsRemoved) {
  auto scope = build_class_scope(stores);
  constexpr std::string_view lambda_class_name =
      "LKotlinLambdaSingletonRemoval$foo$1;";
  constexpr std::string_view root_method_name =
      "LKotlinLambdaSingletonRemoval;.foo:()V";
  set_root_method(root_method_name);

  auto* lambda_class = type_class(DexType::make_type(lambda_class_name));
  ASSERT_THAT(lambda_class, NotNull());
  lambda_class->set_deobfuscated_name(lambda_class_name);

  auto root_method = DexMethod::get_method(root_method_name)->as_def();
  auto code_root = root_method->get_code();
  ASSERT_THAT(code_root, NotNull());
  check_sget_available(code_root);

  auto klr = new KotlinStatelessLambdaSingletonRemovalPass();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  check_sget_not_available(code_root);
  check_removal_result_new_instance(code_root, lambda_class_name);
}

TEST_F(KotlinLambdaOptTest, NoEffectOnNamedClass) {
  auto scope = build_class_scope(stores);
  constexpr std::string_view class_name = "LKotlinInstanceRemovalNamedEquiv;";
  constexpr std::string_view root_method =
      "LKotlinInstanceRemovalNamedEquiv;.bar:()V";

  auto* lambda_class = type_class(DexType::make_type(class_name));
  ASSERT_THAT(lambda_class, NotNull());
  lambda_class->set_deobfuscated_name(class_name);

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

} // namespace
