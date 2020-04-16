/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "DexAccess.h"
#include "MethodMerger.h"
#include "RedexTest.h"
#include "Resolver.h"

using namespace method_merger;

struct MethodMergerTest : RedexTest {
  DexClass* m_cls;
  Scope scope;

  MethodMergerTest() : RedexTest() {
    ClassCreator cc(DexType::make_type("Lfoo;"));
    cc.set_super(type::java_lang_Object());
    m_cls = cc.create();
    scope.push_back(m_cls);
  }

  DexMethod* create_a_simple_method(const std::string& full_descriptor,
                                    DexAccessFlags access,
                                    int ret_value) {
    auto ref = DexMethod::make_method(full_descriptor);
    MethodCreator mc(ref, access);
    auto res_loc = mc.make_local(type::_int());
    auto main_block = mc.get_main_block();
    main_block->load_const(res_loc, ret_value);
    main_block->ret(res_loc);
    auto method = mc.create();
    m_cls->add_method(method);
    return method;
  }
};

TEST_F(MethodMergerTest, merge_methods_within_class) {
  auto access = ACC_PUBLIC | ACC_STATIC;
  // group 1
  auto method0 = create_a_simple_method("Lfoo;.method_0:(I)I", access, 0);
  auto method1 = create_a_simple_method("Lfoo;.method_1:(I)I", access, 1);
  auto method2 = create_a_simple_method("Lfoo;.method_2:(I)I", access, 2);
  // group 2
  auto method3 = create_a_simple_method("Lfoo;.method_3:()I", access, 3);
  auto method4 = create_a_simple_method("Lfoo;.method_4:()I", access, 4);
  auto method5 = create_a_simple_method("Lfoo;.method_5:()I", access, 5);
  DexMethod* method6 = nullptr;
  {
    // method6's proto is different and it calls 0-5 methods.
    MethodCreator mc(DexMethod::make_method("Lfoo;.method_6:()V"), access);
    auto loc = mc.make_local(type::_int());
    auto main_block = mc.get_main_block();
    main_block->load_const(loc, 0);
    // At least two callsites for each methods.
    for (uint32_t i = 0; i < 2; ++i) {
      main_block->invoke(method0, {loc});
      main_block->invoke(method1, {loc});
      main_block->invoke(method2, {loc});
    }
    for (uint32_t i = 0; i < 2; ++i) {
      main_block->invoke(method3, {});
      main_block->invoke(method4, {});
      main_block->invoke(method5, {});
    }
    method6 = mc.create();
    m_cls->add_method(method6);
  }
  auto stats = merge_methods_within_class(scope,
                                          scope,
                                          /*merge_static*/ true,
                                          /*merge_non_virtual*/ true,
                                          /*merge_direct*/ true);

  EXPECT_EQ(stats.num_merged_static_methods, 4);
  EXPECT_EQ(stats.num_merged_nonvirt_methods, 0);
  EXPECT_EQ(stats.num_merged_direct_methods, 0);
  // Now method6 invokes disp_012 and disp_345 instead.
  std::vector<DexMethod*> callees;
  for (auto& mie : InstructionIterable(method6->get_code())) {
    auto insn = mie.insn;
    if (!insn->has_method()) {
      continue;
    }
    auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
    callees.push_back(method);
  }
  ASSERT_EQ(callees.size(), 12);
  DexMethod* disp_012 = *callees.begin();
  for (uint32_t i = 0; i < 6; ++i) {
    EXPECT_EQ(disp_012, callees[i]);
  }
  DexMethod* disp_345 = *(callees.rbegin());
  for (uint32_t i = 6; i < 12; ++i) {
    EXPECT_EQ(disp_345, callees[i]);
  }
}
