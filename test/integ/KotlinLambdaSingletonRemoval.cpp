/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexUtil.h"
#include "KotlinStatelessLambdaSingletonRemovalPass.h"
#include "RedexTest.h"
#include "Resolver.h"
#include "Show.h"

namespace {
using ::testing::Contains;
using ::testing::IsNull;
using ::testing::Not;
using ::testing::NotNull;

class KotlinLambdaSingletonRemovalTest : public RedexIntegrationTest {
 protected:
  void set_root_method(std::string_view full_name) {
    auto* method = DexMethod::get_method(full_name)->as_def();
    ASSERT_NE(nullptr, method);
    method->rstate.set_root();
  }

  auto find_opcode(const ir_list::ConstInstructionIterable& ii,
                   IROpcode opcode) {
    return std::find_if(ii.begin(), ii.end(),
                        [opcode](const MethodItemEntry& mie) {
                          return mie.insn->opcode() == opcode;
                        });
  }

  void check_opcode_present(const IRCode* code, IROpcode opcode) {
    const auto ii = InstructionIterable(code);
    EXPECT_NE(find_opcode(ii, opcode), ii.end())
        << "Opcode " << SHOW(opcode) << " not found in " << SHOW(code);
  }

  void check_opcode_absent(const IRCode* code, IROpcode opcode) {
    const auto ii = InstructionIterable(code);
    EXPECT_EQ(find_opcode(ii, opcode), ii.end())
        << "Opcode " << SHOW(opcode) << " found in " << SHOW(code);
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

TEST_F(KotlinLambdaSingletonRemovalTest, LambdaSingletonIsRemoved) {
  auto scope = build_class_scope(stores);
  constexpr std::string_view lambda_class_name =
      "LKotlinLambdaSingletonRemoval$foo$1;";
  constexpr std::string_view root_method_name =
      "LKotlinLambdaSingletonRemoval;.foo:()V";
  constexpr std::string_view clinit_method_name =
      "LKotlinLambdaSingletonRemoval$foo$1;.<clinit>:()V";
  constexpr std::string_view singleton_field_name =
      "LKotlinLambdaSingletonRemoval$foo$1;.INSTANCE:"
      "LKotlinLambdaSingletonRemoval$foo$1;";
  set_root_method(root_method_name);

  auto* lambda_class = type_class(DexType::make_type(lambda_class_name));
  ASSERT_THAT(lambda_class, NotNull());
  lambda_class->set_deobfuscated_name(lambda_class_name);

  const auto* singleton_field = DexField::get_field(singleton_field_name);
  ASSERT_THAT(singleton_field, NotNull())
      << "Sanity check failed: singleton field not found";
  ASSERT_THAT(lambda_class->get_sfields(), Contains(singleton_field))
      << "Sanity check failed: singleton field not found in "
      << SHOW(lambda_class);

  auto* root_method = DexMethod::get_method(root_method_name)->as_def();
  auto* code_root = root_method->get_code();
  ASSERT_THAT(code_root, NotNull());
  check_opcode_present(code_root, OPCODE_SGET_OBJECT);

  auto* const clinit_method =
      DexMethod::get_method(clinit_method_name)->as_def();
  auto* code_clinit = clinit_method->get_code();
  ASSERT_THAT(code_clinit, NotNull());
  check_opcode_present(code_clinit, OPCODE_SPUT_OBJECT);

  auto* klr = new KotlinStatelessLambdaSingletonRemovalPass();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  check_opcode_absent(code_root, OPCODE_SGET_OBJECT);
  check_removal_result_new_instance(code_root, lambda_class_name);
  ASSERT_THAT(lambda_class->get_sfields(), Not(Contains(singleton_field)))
      << "Singleton field " << SHOW(singleton_field)
      << " is unexpectedly not deleted from " << SHOW(lambda_class);

  code_clinit = clinit_method->get_code();
  ASSERT_THAT(code_clinit, NotNull());
  check_opcode_absent(code_clinit, OPCODE_SPUT_OBJECT);
}

TEST_F(KotlinLambdaSingletonRemovalTest, NoEffectOnNamedClass) {
  auto scope = build_class_scope(stores);
  constexpr std::string_view class_name = "LKotlinInstanceRemovalNamedEquiv;";
  constexpr std::string_view root_method =
      "LKotlinInstanceRemovalNamedEquiv;.bar:()V";
  constexpr std::string_view clinit_method_name =
      "LKotlinInstanceRemovalNamedEquiv;.<clinit>:()V";
  constexpr std::string_view singleton_field_name =
      "LKotlinInstanceRemovalNamedEquiv;.INSTANCE:"
      "LKotlinInstanceRemovalNamedEquiv;";

  auto* lambda_class = type_class(DexType::make_type(class_name));
  ASSERT_THAT(lambda_class, NotNull());
  lambda_class->set_deobfuscated_name(class_name);
  const auto* singleton_field = DexField::get_field(singleton_field_name);
  ASSERT_THAT(singleton_field, NotNull())
      << "Sanity check: Singleton is unexpectedly not found";
  ASSERT_THAT(lambda_class->get_sfields(), Contains(singleton_field))
      << "Sanity check: Singleton is unexpectedly not found in "
      << SHOW(lambda_class);

  set_root_method(root_method);
  auto* y_method = DexMethod::get_method(root_method)->as_def();
  auto* codey = y_method->get_code();
  ASSERT_THAT(codey, NotNull());
  check_opcode_present(codey, OPCODE_SGET_OBJECT);

  auto* const clinit_method =
      DexMethod::get_method(clinit_method_name)->as_def();
  auto* code_clinit = clinit_method->get_code();
  ASSERT_THAT(code_clinit, NotNull());
  check_opcode_present(code_clinit, OPCODE_SPUT_OBJECT);

  auto* klr = new KotlinStatelessLambdaSingletonRemovalPass();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  check_opcode_present(codey, OPCODE_SGET_OBJECT);
  EXPECT_THAT(lambda_class->get_sfields(), Contains(singleton_field))
      << "Singleton field is unexpectedly deleted from " << SHOW(lambda_class);

  code_clinit = clinit_method->get_code();
  ASSERT_THAT(code_clinit, NotNull());
  check_opcode_present(code_clinit, OPCODE_SPUT_OBJECT);
}

class KotlinLambdaSingletonNoopTest
    : public KotlinLambdaSingletonRemovalTest,
      public ::testing::WithParamInterface<
          std::tuple<std::string_view, std::string_view>> {};

TEST_P(KotlinLambdaSingletonNoopTest, main) {
  auto scope = build_class_scope(stores);
  const auto& class_name = std::get<0>(GetParam());
  const auto& root_method_name = std::get<1>(GetParam());

  // Ensure that the class exists so there's errors like typo in class name.
  ASSERT_THAT(type_class(DexType::make_type(class_name)), NotNull())
      << "Class " << class_name << " not found";

  std::ostringstream singleton_field_name_ss;
  singleton_field_name_ss << class_name << ".INSTANCE:" << class_name;
  const auto singleton_field_name = singleton_field_name_ss.str();

  const auto* singleton_field = DexField::get_field(singleton_field_name);
  EXPECT_THAT(singleton_field, IsNull())
      << "Sanity check: Singleton is unexpectedly found";

  const auto root_method = DexMethod::get_method(root_method_name)->as_def();
  ASSERT_THAT(root_method->get_code(), NotNull());
  const auto code_root_pre{*root_method->get_code()};

  auto klr = new KotlinStatelessLambdaSingletonRemovalPass();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  ASSERT_THAT(root_method->get_code(), NotNull());
  const auto code_root_post{*root_method->get_code()};

  const auto pre_iterable = InstructionIterable(code_root_pre);
  const auto post_iterable = InstructionIterable(code_root_post);
  const auto equals =
      std::equal(pre_iterable.begin(), pre_iterable.end(),
                 post_iterable.begin(), post_iterable.end(),
                 [](const MethodItemEntry& a, const MethodItemEntry& b) {
                   EXPECT_EQ(a, b) << "Instructions do not equal";
                   return a == b;
                 });
  EXPECT_TRUE(equals)
      << "Instructions have unexpctedly changed after the pass.\nPre: "
      << SHOW(&code_root_pre) << "\nPost: " << SHOW(&code_root_post);
}

INSTANTIATE_TEST_SUITE_P(
    KotlinLambdaSingletonNoopTests,
    KotlinLambdaSingletonNoopTest,
    ::testing::ValuesIn(
        std::initializer_list<std::tuple<std::string_view, std::string_view>>{
            {"LKotlinStatefulLambda;", "LKotlinStatefulLambda;.foo:()V"},
            {"LKotlinAnonymousClassImplementingFunction$foo$addfn$1;",
             "LKotlinAnonymousClassImplementingFunction;.foo:()V"}}));

} // namespace
