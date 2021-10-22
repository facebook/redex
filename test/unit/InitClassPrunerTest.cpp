/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "InitClassPruner.h"
#include "RedexTest.h"
#include "Show.h"
#include "VirtualScope.h"

struct InitClassPrunerTest : public RedexTest {
  DexType* a_type;
  DexType* b_type;
  DexType* c_type;
  DexType* d_type;
  InitClassPrunerTest() {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    get_vmethods(type::java_lang_Object());

    a_type = DexType::make_type("LA;");
    b_type = DexType::make_type("LB;");
    c_type = DexType::make_type("LC;");
    d_type = DexType::make_type("LD;");
    ClassCreator a_creator(a_type);
    a_creator.set_super(type::java_lang_Object());
    ClassCreator b_creator(b_type);
    b_creator.set_super(a_type);
    ClassCreator c_creator(c_type);
    c_creator.set_super(b_type);
    ClassCreator d_creator(d_type);
    d_creator.set_super(c_type);
    a_creator.create();
    b_creator.create();
    c_creator.create();
    d_creator.create();
    add_clinit(b_type);
    add_clinit(c_type);
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

  void run_init_class_pruner(IRCode* ircode) {
    Scope scope{type_class(type::java_lang_Object()), type_class(a_type),
                type_class(b_type), type_class(c_type), type_class(d_type)};
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ true);
    ircode->build_cfg();
    init_classes::InitClassPruner init_class_pruner(
        init_classes_with_side_effects, type::java_lang_Object(),
        ircode->cfg());
    init_class_pruner.apply();
    ircode->clear_cfg();
  }

  ~InitClassPrunerTest() {}
};

TEST_F(InitClassPrunerTest, remove_if_no_side_effects) {
  auto code = assembler::ircode_from_string(R"(
    (
      (init-class "LA;")
      (return-void)
    )
  )");
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
  )");

  run_init_class_pruner(code.get());
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(InitClassPrunerTest, keep_if_side_effects) {
  auto code = assembler::ircode_from_string(R"(
    (
      (init-class "LB;")
      (return-void)
    )
  )");
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (init-class "LB;")
      (return-void)
    )
  )");

  run_init_class_pruner(code.get());
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(InitClassPrunerTest, refine_if_base_side_effects) {
  auto code = assembler::ircode_from_string(R"(
    (
      (init-class "LD;")
      (return-void)
    )
  )");
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (init-class "LC;")
      (return-void)
    )
  )");

  run_init_class_pruner(code.get());
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(InitClassPrunerTest, remove_reundant_forward) {
  auto code = assembler::ircode_from_string(R"(
    (
      (init-class "LC;")
      (init-class "LB;")
      (return-void)
    )
  )");
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (init-class "LC;")
      (return-void)
    )
  )");

  run_init_class_pruner(code.get());
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
