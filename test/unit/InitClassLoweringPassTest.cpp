/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <sstream>

#include "Creators.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "InitClassLoweringPass.h"
#include "RedexTest.h"
#include "ScopeHelper.h"
#include "Show.h"
#include "VirtualScope.h"

class InitClassLoweringPassTest : public RedexTest {
 public:
  ::sparta::s_expr run_pass(const std::string& code) {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    virt_scope::get_vmethods(type::java_lang_Object());

    auto a_type = DexType::make_type("LA;");
    auto b_type = DexType::make_type("LB;");
    auto c_type = DexType::make_type("LC;");
    auto d_type = DexType::make_type("LD;");
    ClassCreator a_creator(a_type);
    a_creator.set_super(type::java_lang_Object());
    ClassCreator b_creator(b_type);
    b_creator.set_super(type::java_lang_Object());
    ClassCreator c_creator(c_type);
    c_creator.set_super(type::java_lang_Object());
    ClassCreator d_creator(d_type);
    d_creator.set_super(type::java_lang_Object());
    auto a_cls = a_creator.create();
    auto b_cls = b_creator.create();
    auto c_cls = c_creator.create();
    auto d_cls = d_creator.create();
    add_clinit(a_type);
    add_sfield(a_type, type::_int());
    add_clinit(b_type);
    add_clinit(c_type);
    add_sfield(c_type, type::_double());

    std::string class_name = "LTest;";
    ClassCreator creator(DexType::make_type(class_name));
    creator.set_super(type::java_lang_Object());
    auto signature = class_name + ".foo:()V";
    auto method = DexMethod::make_method(signature)->make_concrete(
        ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(assembler::ircode_from_string(code));
    creator.add_method(method);

    InitClassLoweringPass pass;
    PassManager manager({&pass});
    ConfigFiles config(Json::nullValue);
    config.parse_global_config();
    DexStore store("classes");
    store.add_classes({a_cls, b_cls, c_cls, d_cls, creator.create()});
    std::vector<DexStore> stores;
    stores.emplace_back(std::move(store));
    manager.run_passes(stores, config);

    return assembler::to_s_expr(method->get_code());
  }

  void add_clinit(DexType* type) {
    auto clinit_name = DexString::make_string("<clinit>");
    auto void_args = DexTypeList::make_type_list({});
    auto void_void = DexProto::make_proto(type::_void(), void_args);
    auto clinit = static_cast<DexMethod*>(
        DexMethod::make_method(type, clinit_name, void_void));
    clinit->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
    clinit->set_code(std::make_unique<IRCode>());
    auto code = clinit->get_code();
    auto method = DexMethod::make_method("Lunknown;.unknown:()V");
    code->push_back(dex_asm::dasm(OPCODE_INVOKE_STATIC, method, {}));
    code->push_back(dex_asm::dasm(OPCODE_RETURN_VOID));
    type_class(type)->add_method(clinit);
  }

  void add_sfield(DexType* type, DexType* field_type) {
    auto sfield_name = DexString::make_string("existing_field");
    auto field = static_cast<DexField*>(
        DexField::make_field(type, sfield_name, field_type));
    field->make_concrete(ACC_PUBLIC | ACC_STATIC);
    type_class(type)->add_field(field);
  }

  ::sparta::s_expr get_s_expr(const std::string& code) {
    return assembler::to_s_expr(assembler::ircode_from_string(code).get());
  }

  ::testing::AssertionResult run_test(const std::string& input,
                                      const std::string& expected) {
    auto actual_s_expr = run_pass(input);
    auto expected_s_expr = get_s_expr(expected);
    if (actual_s_expr == expected_s_expr) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << input << "\nevaluates to\n"
           << actual_s_expr.str() << "\ninstead of\n"
           << expected_s_expr.str();
  }
};

TEST_F(InitClassLoweringPassTest, existing_field) {
  auto original_code = R"(
     (
      (init-class "LA;")
      (return-void)
     )
    )";
  auto expected_code = R"(
     (
      (sget "LA;.existing_field:I")
      (move-result-pseudo v0)
      (return-void)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

TEST_F(InitClassLoweringPassTest, added_field) {
  auto original_code = R"(
     (
      (init-class "LB;")
      (return-void)
     )
    )";
  auto expected_code = R"(
     (
      (sget-object "LB;.$redex_init_class:LB;")
      (move-result-pseudo-object v0)
      (return-void)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

TEST_F(InitClassLoweringPassTest, wide_field) {
  auto original_code = R"(
     (
      (init-class "LC;")
      (return-void)
     )
    )";
  auto expected_code = R"(
     (
      (sget-wide "LC;.existing_field:D")
      (move-result-pseudo-wide v0)
      (return-void)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}

TEST_F(InitClassLoweringPassTest, no_side_effects) {
  auto original_code = R"(
     (
      (init-class "LD;")
      (return-void)
     )
    )";
  auto expected_code = R"(
     (
      (return-void)
     )
    )";
  ASSERT_TRUE(run_test(original_code, expected_code));
}
