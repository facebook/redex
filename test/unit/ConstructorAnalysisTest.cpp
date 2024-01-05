/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApiLevelChecker.h"
#include "ConstructorAnalysis.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"

struct ConstructorAnalysisTest : public RedexTest {};

DexClass* create_a_class(const char* description) {
  ClassCreator cc(DexType::make_type(description));
  cc.set_super(type::java_lang_Object());
  return cc.create();
}

/**
 * Create a method like
 * void <init>(object, ..(num_param_types many).., object) {
 *   load-param v0
 *   ...
 *   load-null v1
 *   iput f, v1, v0
 *   ...
 *   load-null v1
 *   invoke-direct v0, v1, ..(num_param_types many).., v1, {{init_to_call}}
 *   ret;
 * }
 */
DexMethod* create_an_init_method(
    DexClass* cls,
    DexMethodRef* init_to_call,
    size_t num_param_types,
    const std::vector<DexField*>& fields_to_assign_null = {},
    bool before_init_call = false,
    bool spurious_init_call = false) {
  DexTypeList::ContainerType param_types;
  auto java_lang_Object = DexType::make_type("Ljava/lang/Object;");
  for (size_t i = 0; i < num_param_types; i++) {
    param_types.push_back(java_lang_Object);
  }
  auto proto = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list(std::move(param_types)));
  auto ref = DexMethod::make_method(cls->get_type(),
                                    DexString::make_string("<init>"), proto);
  MethodCreator mc(ref, ACC_PUBLIC | ACC_CONSTRUCTOR);
  auto args = mc.get_reg_args();
  auto first_arg = *args.begin();
  auto main_block = mc.get_main_block();
  if (before_init_call) {
    for (auto f : fields_to_assign_null) {
      auto f_null_loc = mc.make_local(f->get_type());
      main_block->load_null(f_null_loc);
      main_block->iput(f, first_arg, f_null_loc);
    }
  }
  std::vector<Location> init_args{first_arg};
  auto p_null_loc = mc.make_local(java_lang_Object);
  main_block->load_null(p_null_loc);
  for (size_t i = 0; i < num_param_types; i++) {
    init_args.push_back(p_null_loc);
  }
  main_block->invoke(OPCODE_INVOKE_DIRECT, init_to_call, init_args);
  for (auto f : fields_to_assign_null) {
    auto f_null_loc = mc.make_local(f->get_type());
    main_block->load_null(f_null_loc);
    main_block->iput(f, first_arg, f_null_loc);
  }
  if (spurious_init_call) {
    init_args[0] = mc.make_local(init_to_call->get_class());
    main_block->new_instance(init_to_call->get_class(), init_args[0]);
    main_block->invoke(OPCODE_INVOKE_DIRECT, init_to_call, init_args);
  }
  main_block->ret_void();
  auto method = mc.create();
  cls->add_method(method);
  method->get_code()->build_cfg();
  return method;
}

TEST_F(ConstructorAnalysisTest, can_inline_init_simple) {
  auto foo_cls = create_a_class("Lfoo;");
  auto foo_init1 = create_an_init_method(
      foo_cls, DexMethod::make_method("Ljava/lang/Object;.<init>:()V"), 0, {});
  auto foo_init2 = create_an_init_method(foo_cls, foo_init1, 1, {});

  EXPECT_FALSE(constructor_analysis::can_inline_init(foo_init1));
  EXPECT_TRUE(constructor_analysis::can_inline_init(foo_init2));
  EXPECT_TRUE(constructor_analysis::can_inline_inits_in_same_class(
      foo_init2, foo_init1, nullptr));
}

TEST_F(ConstructorAnalysisTest, can_inline_init_iput_before_init_call) {
  auto foo_cls = create_a_class("Lfoo;");
  auto f = DexField::make_field("Lfoo;.f:Ljava/lang/Object;")
               ->make_concrete(ACC_PUBLIC);
  auto foo_init1 = create_an_init_method(
      foo_cls, DexMethod::make_method("Ljava/lang/Object;.<init>:()V"), 0, {});
  auto foo_init2 = create_an_init_method(foo_cls, foo_init1, 1, {f},
                                         /* before_init_call */ true);

  EXPECT_FALSE(constructor_analysis::can_inline_init(foo_init2));
}

TEST_F(ConstructorAnalysisTest, can_inline_init_iput_after_init_call) {
  auto foo_cls = create_a_class("Lfoo;");
  auto f = DexField::make_field("Lsfoo;.f:Ljava/lang/Object;")
               ->make_concrete(ACC_PUBLIC);
  auto foo_init1 = create_an_init_method(
      foo_cls, DexMethod::make_method("Ljava/lang/Object;.<init>:()V"), 0, {});
  auto foo_init2 = create_an_init_method(foo_cls, foo_init1, 1, {f},
                                         /* before_init_call */ false);

  EXPECT_TRUE(constructor_analysis::can_inline_init(foo_init2));
}

TEST_F(ConstructorAnalysisTest,
       can_inline_inits_in_same_class_unsupported_init_call) {
  auto foo_cls = create_a_class("Lfoo;");
  auto foo_init1 = create_an_init_method(
      foo_cls, DexMethod::make_method("Ljava/lang/Object;.<init>:()V"), 0, {});
  auto foo_init2 = create_an_init_method(foo_cls, foo_init1, 1, {}, false,
                                         /* spurious_init_call */ true);

  std::vector<IRInstruction*> callsite_insns;
  for (auto& mie : InstructionIterable(foo_init2->get_code()->cfg())) {
    IRInstruction* insn = mie.insn;
    if (insn->opcode() == OPCODE_INVOKE_DIRECT &&
        insn->get_method() == foo_init1) {
      callsite_insns.push_back(insn);
    }
  }
  EXPECT_EQ(callsite_insns.size(), 2);

  EXPECT_FALSE(constructor_analysis::can_inline_inits_in_same_class(
      foo_init2, foo_init1, nullptr));
  EXPECT_TRUE(constructor_analysis::can_inline_inits_in_same_class(
      foo_init2, foo_init1, callsite_insns[0]));
  EXPECT_FALSE(constructor_analysis::can_inline_inits_in_same_class(
      foo_init2, foo_init1, callsite_insns[1]));
}
