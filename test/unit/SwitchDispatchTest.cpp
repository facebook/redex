/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "RedexTest.h"
#include "SwitchDispatch.h"

DexMethod* make_a_method(const std::string& full_descriptor,
                         DexAccessFlags acc) {
  auto ref = DexMethod::make_method(full_descriptor);
  MethodCreator mc(ref, acc);
  return mc.create();
}

class SwitchDispatchTest : public RedexTest {};

TEST_F(SwitchDispatchTest, create_simple_dispatch) {
  ClassCreator cc(DexType::make_type("Lfoo;"));
  cc.set_super(type::java_lang_Object());
  cc.create();
  { // static methods
    DexAccessFlags access = ACC_STATIC;
    auto method_a = make_a_method("Lfoo;.a:(I)I", access);
    auto method_b = make_a_method("Lfoo;.b:(I)I", access);
    auto method_c = make_a_method("Lfoo;.c:(I)I", access);
    std::map<SwitchIndices, DexMethod*> indices_to_callee{
        {{0}, method_a}, {{1}, method_b}, {{2}, method_c}};
    auto method = dispatch::create_simple_dispatch(indices_to_callee);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->get_access(), ACC_STATIC | ACC_PUBLIC);
    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    EXPECT_EQ(cfg.num_blocks(), 4);
  }
  { // virtual methods
    DexAccessFlags access = ACC_PUBLIC;
    auto method_a = make_a_method("Lfoo;.a:()V", access | ACC_SYNTHETIC);
    auto method_b = make_a_method("Lfoo;.b:()V", access);
    std::map<SwitchIndices, DexMethod*> indices_to_callee{{{0}, method_a},
                                                          {{1}, method_b}};
    auto method = dispatch::create_simple_dispatch(indices_to_callee);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->get_access(), ACC_PUBLIC);
  }
  {
    // direct methods
    DexAccessFlags access = ACC_PRIVATE;
    auto method_a = make_a_method("Lfoo;.aa:()V", access);
    auto method_b = make_a_method("Lfoo;.bb:()V", access);
    std::map<SwitchIndices, DexMethod*> indices_to_callee{{{0}, method_a},
                                                          {{1}, method_b}};
    auto method = dispatch::create_simple_dispatch(indices_to_callee);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->get_access(), ACC_PRIVATE);
  }
  {
    // mix
    auto method_a = make_a_method("Lfoo;.aaa:()V", ACC_PUBLIC);
    auto method_b = make_a_method("Lfoo;.bbb:()V", ACC_PRIVATE);
    std::map<SwitchIndices, DexMethod*> indices_to_callee{{{0}, method_a},
                                                          {{1}, method_b}};
    auto method = dispatch::create_simple_dispatch(indices_to_callee);
    ASSERT_EQ(method, nullptr);
  }
}
