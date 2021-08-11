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

  MethodMergerTest() {
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
