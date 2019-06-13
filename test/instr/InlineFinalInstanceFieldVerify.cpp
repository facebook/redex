/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <boost/any.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "IRCode.h"
#include "Resolver.h"
#include "Show.h"
#include "VerifyUtil.h"

int count_igets(cfg::ControlFlowGraph& cfg) {
  size_t num_igets = 0;
  for (const auto& mie : InstructionIterable(cfg)) {
    if (is_iget(mie.insn->opcode())) {
      num_igets++;
    }
  }
  return num_igets;
}

void expect_class_have_num_init(DexClasses& classes,
                                const char* name,
                                int num_of_init) {
  auto cls = find_class_named(classes, name);
  ASSERT_NE(nullptr, cls);
  auto ctors = cls->get_ctors();
  EXPECT_EQ(ctors.size(), num_of_init);
}

std::unordered_set<std::string> get_fields_name_accessed(DexMethod* method) {
  std::unordered_set<std::string> return_names;
  for (auto& insn : method->get_dex_code()->get_instructions()) {
    if (insn->has_field()) {
      return_names.emplace(
          ((DexOpcodeField*)insn)->get_field()->get_name()->c_str());
    }
  }
  return return_names;
}

int get_class_num_ifields(DexClasses& classes, const char* name) {
  auto cls = find_class_named(classes, name);
  if (cls == nullptr) {
    return -1;
  }
  auto ifields = cls->get_ifields();
  return ifields.size();
}

bool field_name_eq(DexField* field, const char* name) {
  return strcmp(name, field->get_name()->c_str()) == 0;
}

/*
 * Ensure that we are actually replacing inlineable instance fields by checking
 * that they exist in the pre-redexed binary.
 */

TEST_F(PreVerify, InlineFinalInstanceField) {
  expect_class_have_num_init(classes, "Lredex/EncodableFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/NotFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/UnEncodableFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/HasCharSequenceFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/OneInitCanReplaceFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/OneInitCantReplaceFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/TwoInitCantReplaceFinal;", 2);
  expect_class_have_num_init(classes, "Lredex/MixedTypeInstance;", 1);

  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/EncodableFinal;"), 8);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/NotFinal;"), 8);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/UnEncodableFinal;"), 1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/HasCharSequenceFinal;"), 1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/OneInitCanReplaceFinal;"),
            1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/OneInitCantReplaceFinal;"),
            1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/TwoInitCantReplaceFinal;"),
            1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/MixedTypeInstance;"), 10);

  auto cls = find_class_named(classes, "Lredex/MixedTypeInstance;");

  auto method = find_vmethod_named(*cls, "change0");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  auto field_names = get_fields_name_accessed(method);
  EXPECT_THAT(
      field_names,
      ::testing::UnorderedElementsAre("m_changed_0", "m_final_accessed"));

  method = find_vmethod_named(*cls, "change2");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_THAT(
      field_names,
      ::testing::UnorderedElementsAre("m_changed_2", "m_final_accessed"));

  method = find_vmethod_named(*cls, "change4");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_THAT(
      field_names,
      ::testing::UnorderedElementsAre("m_changed_4", "m_non_final_accessed"));

  method = find_vmethod_named(*cls, "change5");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_THAT(
      field_names,
      ::testing::UnorderedElementsAre("m_changed_5", "m_non_final_accessed"));

  method = find_vmethod_named(*cls, "return_final_inlineable");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_THAT(field_names,
              ::testing::UnorderedElementsAre("m_final_inlineable"));

  method = find_vmethod_named(*cls, "return_non_final_inlineable");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_THAT(field_names,
              ::testing::UnorderedElementsAre("m_non_final_inlineable"));

  auto read_ctors_cls1 = find_class_named(classes, "Lredex/ReadInCtors1;");
  auto read_ctors_cls2 = find_class_named(classes, "Lredex/ReadInCtors2;");

  // Ctors of both read ctors class have one iget.
  for (auto& meth : read_ctors_cls1->get_dmethods()) {
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    EXPECT_EQ(1, count_igets(code->cfg()));
  }
  for (auto& meth : read_ctors_cls2->get_dmethods()) {
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    EXPECT_EQ(1, count_igets(code->cfg()));
  }

  // 3 igets in test methods.
  auto test_cls =
      find_class_named(classes, "Lredex/InlineFinalInstanceFieldTest;");
  size_t count = 0;
  for (auto& meth : test_cls->get_vmethods()) {
    if (meth->get_name()->str() != "testReadInCtors") {
      continue;
    }
    ++count;
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    EXPECT_EQ(3, count_igets(code->cfg()));
  }
  // Make sure there is a testReadInCtors function
  ASSERT_EQ(count, 1);
}

/*
 * Ensure that we've removed the appropriate instance fields.
 */
TEST_F(PostVerify, InlineFinalInstanceField) {
  // Even though fields are all inlined, the <init> function should still
  // exist
  expect_class_have_num_init(classes, "Lredex/EncodableFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/NotFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/UnEncodableFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/HasCharSequenceFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/OneInitCanReplaceFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/OneInitCantReplaceFinal;", 1);
  expect_class_have_num_init(classes, "Lredex/TwoInitCantReplaceFinal;", 2);
  expect_class_have_num_init(classes, "Lredex/MixedTypeInstance;", 1);

  // Because ifield have no DexEncodedValue so they will be assigned in <init>
  // then been referenced in code and can't be removed.
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/EncodableFinal;"), 8);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/NotFinal;"), 8);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/UnEncodableFinal;"), 1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/HasCharSequenceFinal;"), 1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/OneInitCanReplaceFinal;"),
            1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/OneInitCantReplaceFinal;"),
            1);
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/TwoInitCantReplaceFinal;"),
            1);

  // Because m_deletable was only assigned 0 in <init>, which is equal to its
  // default value so it's iput in <init> function can be removed, then there
  // is no other references to m_deletable, so it will be removed by RMU.
  // Other fields will be remained.
  EXPECT_EQ(get_class_num_ifields(classes, "Lredex/MixedTypeInstance;"), 9);

  auto cls = find_class_named(classes, "Lredex/MixedTypeInstance;");

  auto method = find_vmethod_named(*cls, "change0");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  auto field_names = get_fields_name_accessed(method);
  EXPECT_THAT(
      field_names,
      ::testing::UnorderedElementsAre("m_changed_0", "m_final_accessed"));

  method = find_vmethod_named(*cls, "change2");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_THAT(
      field_names,
      ::testing::UnorderedElementsAre("m_changed_2", "m_final_accessed"));

  method = find_vmethod_named(*cls, "change4");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_THAT(
      field_names,
      ::testing::UnorderedElementsAre("m_changed_4", "m_non_final_accessed"));

  method = find_vmethod_named(*cls, "change5");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_THAT(
      field_names,
      ::testing::UnorderedElementsAre("m_changed_5", "m_non_final_accessed"));

  // Because m_final_inlineable and m_non_final_inlineable were not accessed
  // from method invoked in <init> function, and they fulfill other requirement
  // of inlineable ifields, so their appearance in functions other than <init>
  // function will be inlined.

  method = find_vmethod_named(*cls, "return_final_inlineable");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_EQ(field_names.size(), 0);

  method = find_vmethod_named(*cls, "return_non_final_inlineable");
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, method->get_dex_code());
  field_names = get_fields_name_accessed(method);
  EXPECT_EQ(field_names.size(), 0);

  auto read_ctors_cls1 = find_class_named(classes, "Lredex/ReadInCtors1;");
  auto read_ctors_cls2 = find_class_named(classes, "Lredex/ReadInCtors2;");

  // Ctors of both read ctors class have one iget.
  for (auto& meth : read_ctors_cls1->get_dmethods()) {
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    EXPECT_EQ(0, count_igets(code->cfg()));
  }
  for (auto& meth : read_ctors_cls2->get_dmethods()) {
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    EXPECT_EQ(0, count_igets(code->cfg()));
  }

  // 3 igets in test methods.
  auto test_cls =
      find_class_named(classes, "Lredex/InlineFinalInstanceFieldTest;");
  size_t count = 0;
  for (auto& meth : test_cls->get_vmethods()) {
    if (meth->get_name()->str() != "testReadInCtors") {
      continue;
    }
    ++count;
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    EXPECT_EQ(0, count_igets(code->cfg()));
  }
  // Make sure there is a testReadInCtors function
  ASSERT_EQ(count, 1);
}
