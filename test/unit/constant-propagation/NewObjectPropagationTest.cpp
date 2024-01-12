/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "AbstractDomainPropertyTest.h"
#include "ConstantPropagationTestUtil.h"
#include "Creators.h"
#include "IRAssembler.h"

struct Constants {
  IRInstruction* new_object_insn1 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                                        ->set_type(DexType::make_type("LFoo;"));
  IRInstruction* new_object_insn2 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                                        ->set_type(DexType::make_type("LFoo;"));
  IRInstruction* new_array_insn1 = (new IRInstruction(OPCODE_NEW_ARRAY))
                                       ->set_type(DexType::make_type("[LFoo;"))
                                       ->set_srcs_size(1)
                                       ->set_src(0, 0);
  IRInstruction* new_array_insn2 = (new IRInstruction(OPCODE_NEW_ARRAY))
                                       ->set_type(DexType::make_type("[LFoo;"))
                                       ->set_srcs_size(1)
                                       ->set_src(0, 0);

  NewObjectDomain new_object1{NewObjectDomain(new_object_insn1)};
  NewObjectDomain new_object2{NewObjectDomain(new_object_insn2)};
  NewObjectDomain new_array1_0{
      NewObjectDomain(new_array_insn1, SignedConstantDomain(0))};
  NewObjectDomain new_array1_1{
      NewObjectDomain(new_array_insn1, SignedConstantDomain(1))};
  NewObjectDomain new_array1_2{NewObjectDomain(
      new_array_insn1, SignedConstantDomain(sign_domain::Interval::GEZ))};
  NewObjectDomain new_array2_0{
      NewObjectDomain(new_array_insn2, SignedConstantDomain(0))};
  NewObjectDomain new_array2_1{
      NewObjectDomain(new_array_insn2, SignedConstantDomain(1))};
  NewObjectDomain new_array2_2{NewObjectDomain(
      new_array_insn2, SignedConstantDomain(sign_domain::Interval::GEZ))};
};

template <>
void AbstractDomainPropertyTest<NewObjectDomain>::SetUpTestCase() {
  g_redex = new RedexContext();
}

template <>
void AbstractDomainPropertyTest<NewObjectDomain>::TearDownTestCase() {
  delete g_redex;
}

INSTANTIATE_TYPED_TEST_CASE_P(NewObjectDomain,
                              AbstractDomainPropertyTest,
                              NewObjectDomain);

template <>
std::vector<NewObjectDomain>
AbstractDomainPropertyTest<NewObjectDomain>::non_extremal_values() {
  Constants constants;
  return {constants.new_object1,  constants.new_object2,
          constants.new_array1_0, constants.new_array1_1,
          constants.new_array1_2, constants.new_array2_0,
          constants.new_array2_1, constants.new_array2_2};
}

using NewObjectAnalyzer =
    InstructionAnalyzerCombiner<cp::NewObjectAnalyzer, cp::PrimitiveAnalyzer>;

struct NewObjectTest : public ConstantPropagationTest {
  cp::ImmutableAttributeAnalyzerState m_immut_analyzer_state;
  NewObjectAnalyzer m_analyzer;

 public:
  NewObjectTest() {
    ClassCreator creator(DexType::make_type("Ljava/lang/String;"));
    creator.set_super(type::java_lang_Object());
    creator.set_external();

    auto equals = DexMethod::make_method(
                      "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
                      ->make_concrete(ACC_PUBLIC, true);
    auto hashCode = DexMethod::make_method("Ljava/lang/String;.hashCode:()I")
                        ->make_concrete(ACC_PUBLIC, true);
    creator.add_method(equals);
    creator.add_method(hashCode);

    creator.create();

    m_analyzer = NewObjectAnalyzer(&m_immut_analyzer_state, nullptr);
  }
};

using NewObjectAnalyzer =
    InstructionAnalyzerCombiner<cp::NewObjectAnalyzer, cp::PrimitiveAnalyzer>;

TEST_F(NewObjectTest, two_new_instances_neq) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LA;")
      (move-result-pseudo-object v0)
      (new-instance "LB;")
      (move-result-pseudo-object v1)
      (if-ne v0 v1 :exit)
      (move-object v0 v1)
      (:exit)
      (return v0)
    )
)");

  do_const_prop(code.get(), m_analyzer);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "LA;")
      (move-result-pseudo-object v0)
      (new-instance "LB;")
      (move-result-pseudo-object v1)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(NewObjectTest, same_new_instance_cannot_decide_eq) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LA;")
      (move-result-pseudo-object v0)
      (if-eq v0 v0 :exit)
      (const v0 0)
      (:exit)
      (return v0)
    )
)");

  do_const_prop(code.get(), m_analyzer);

  auto& expected_code = code;

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(NewObjectTest, new_array_neq) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v1 10)
      (new-array v1 "[LA;")
      (move-result-pseudo-object v0)
      (new-array v1 "[LB;")
      (move-result-pseudo-object v1)
      (if-ne v0 v1 :exit)
      (move-object v0 v1)
      (:exit)
      (return v0)
    )
)");

  do_const_prop(code.get(), m_analyzer);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v1 10)
      (new-array v1 "[LA;")
      (move-result-pseudo-object v0)
      (new-array v1 "[LB;")
      (move-result-pseudo-object v1)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(NewObjectTest, instance_of) {
  ClassCreator creator(DexType::make_type("LA;"));
  creator.set_super(type::java_lang_Object());
  creator.create();

  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "LA;")
      (move-result-pseudo-object v0)
      (instance-of v0 "LA;")
      (move-result-pseudo v0)
      (return v0)
    )
)");

  do_const_prop(code.get(), m_analyzer);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "LA;")
      (move-result-pseudo-object v0)
      (const v0 1)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(NewObjectTest, new_array_length) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 10)
      (new-array v0 "[LA;")
      (move-result-pseudo-object v0)
      (array-length v0)
      (move-result-pseudo v0)
      (return v0)
    )
)");

  do_const_prop(code.get(), m_analyzer);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 10)
      (new-array v0 "[LA;")
      (move-result-pseudo-object v0)
      (const v0 10)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(NewObjectTest, filled_new_array_length) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 10)
      (filled-new-array (v0 v0 v0) "[I;")
      (move-result-pseudo-object v0)
      (array-length v0)
      (move-result-pseudo v0)
      (return v0)
    )
)");

  do_const_prop(code.get(), m_analyzer);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 10)
      (filled-new-array (v0 v0 v0) "[I;")
      (move-result-pseudo-object v0)
      (const v0 3)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
