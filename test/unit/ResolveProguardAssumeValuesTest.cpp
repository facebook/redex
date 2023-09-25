/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "ResolveProguardAssumeValues.h"
#include "ScopeHelper.h"
#include "Show.h"

class ResolveProguardAssumeValuesTest : public RedexTest {

 public:
  DexClass* create_class_local(const char* class_name) {
    DexType* type = DexType::make_type(class_name);
    DexClass* cls = create_class(type, type::java_lang_Object(), {},
                                 ACC_PUBLIC | ACC_INTERFACE);
    return cls;
  }

  DexMethod* create_method(DexClass* cls,
                           const char* method_name,
                           DexAccessFlags access) {
    auto proto =
        DexProto::make_proto(type::_boolean(), DexTypeList::make_type_list({}));
    DexMethod* method =
        DexMethod::make_method(cls->get_type(),
                               DexString::make_string(method_name), proto)
            ->make_concrete(access, false);
    method->set_code(std::make_unique<IRCode>(method, 1));
    cls->add_method(method);
    return method;
  }
};

void test(const std::string& code_str, const std::string& expected_str) {

  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);
  auto code_ptr = code.get();
  code_ptr->build_cfg();
  ResolveProguardAssumeValuesPass::process_for_code(code_ptr->cfg());
  auto& cfg = code_ptr->cfg();
  std::cerr << "after:" << std::endl << SHOW(cfg);

  auto expected_ptr = expected.get();
  expected_ptr->build_cfg();
  auto& expected_cfg = expected_ptr->cfg();
  std::cerr << "expected:" << std::endl << SHOW(expected_cfg);
  code_ptr->clear_cfg();
  expected_ptr->clear_cfg();

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()));
}

TEST_F(ResolveProguardAssumeValuesTest, simple) {
  DexClass* classA = create_class_local("LCls;");
  DexMethod* method_a = create_method(classA, "max", ACC_PUBLIC | ACC_STATIC);
  keep_rules::AssumeReturnValue val;
  val.value_type = keep_rules::AssumeReturnValue::ValueType::ValueBool;
  val.value.v = 1;
  g_redex->set_return_value(method_a, val);
  const auto& code_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (invoke-static () "LCls;.max:()Z")
      (move-result v1)
      (goto :end)
      (:true)
      (invoke-static () "LCls;.max:()Z")
      (move-result v1)
      (:end)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (invoke-static () "LCls;.max:()Z")
      (const v1 1)
      (goto :end)
      (:true)
      (invoke-static () "LCls;.max:()Z")
      (const v1 1)
      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str);
}

TEST_F(ResolveProguardAssumeValuesTest, simple_negative) {
  DexClass* classA = create_class_local("LCls;");
  DexMethod* method_a = create_method(classA, "max", ACC_PUBLIC | ACC_STATIC);
  keep_rules::AssumeReturnValue val;
  val.value_type = keep_rules::AssumeReturnValue::ValueType::ValueBool;
  val.value.v = 0;
  g_redex->set_return_value(method_a, val);
  const auto& code_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (invoke-static () "LCls;.max:()Z")
      (move-result v1)
      (goto :end)
      (:true)
      (invoke-static () "LCls;.max:()Z")
      (move-result v1)
      (:end)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (invoke-static () "LCls;.max:()Z")
      (const v1 0)
      (goto :end)
      (:true)
      (invoke-static () "LCls;.max:()Z")
      (const v1 0)
      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str);
}

TEST_F(ResolveProguardAssumeValuesTest, simple_method_not_known) {
  DexClass* classA = create_class_local("LCls;");
  DexMethod* method_a = create_method(classA, "max", ACC_PUBLIC | ACC_STATIC);
  keep_rules::AssumeReturnValue val;
  val.value_type = keep_rules::AssumeReturnValue::ValueType::ValueBool;
  val.value.v = 0;
  g_redex->set_return_value(method_a, val);
  const auto& code_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (invoke-static () "LCls;.max_2:()Z")
      (move-result v1)
      (goto :end)
      (:true)
      (invoke-static () "LCls;.max_2:()Z")
      (move-result v1)
      (:end)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (invoke-static () "LCls;.max_2:()Z")
      (move-result v1)
      (goto :end)
      (:true)
      (invoke-static () "LCls;.max_2:()Z")
      (move-result v1)
      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str);
}

TEST_F(ResolveProguardAssumeValuesTest, field_simple_bool) {
  DexClass* classA = create_class_local("LCls;");
  auto field = static_cast<DexField*>(DexField::make_field("LCls;.f:J"));
  field->make_concrete(ACC_PUBLIC | ACC_STATIC,
                       DexEncodedValue::zero_for_type(field->get_type()));
  classA->add_field(field);
  keep_rules::AssumeReturnValue val;
  val.value_type = keep_rules::AssumeReturnValue::ValueType::ValueBool;
  val.value.v = 1;
  g_redex->set_field_value(field, val);
  const auto& code_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (sget-boolean "LCls;.f:J")
      (move-result v1)
      (goto :end)
      (:true)
      (sget-boolean "LCls;.f:J")
      (move-result v1)
      (:end)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (sget-boolean "LCls;.f:J")
      (const v1 1)
      (goto :end)
      (:true)
      (sget-boolean "LCls;.f:J")
      (const v1 1)
      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str);
}

TEST_F(ResolveProguardAssumeValuesTest, field_simple_bool_with_no_rule) {
  DexClass* classA = create_class_local("LCls;");
  auto field = static_cast<DexField*>(DexField::make_field("LCls;.f:J"));
  field->make_concrete(ACC_PUBLIC | ACC_STATIC,
                       DexEncodedValue::zero_for_type(field->get_type()));
  classA->add_field(field);
  const auto& code_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (sget-boolean "LCls;.f:J")
      (move-result v1)
      (goto :end)
      (:true)
      (sget-boolean "LCls;.f:J")
      (move-result v1)
      (:end)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const-wide v3 2)
      (const-wide v0 10)
      (if-ge v3 v0 :true)
      (sget-boolean "LCls;.f:J")
      (move-result v1)
      (goto :end)
      (:true)
      (sget-boolean "LCls;.f:J")
      (move-result v1)
      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str);
}
