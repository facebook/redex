/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexUtil.h"
#include "Transform.h"

TEST(SimpleInlineTest, hasAliasedArgs) {
  g_redex = new RedexContext();
  using namespace dex_asm;
  auto args = DexTypeList::make_type_list({});
  auto proto = DexProto::make_proto(get_void_type(), args);
  auto callee = DexMethod::make_method(
      get_object_type(), DexString::make_string("testCallee"), proto);
  callee->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::make_unique<DexCode>(), false);
  callee->get_code()->set_registers_size(2);
  callee->get_code()->set_ins_size(2);
  auto caller = DexMethod::make_method(
      get_object_type(), DexString::make_string("testCaller"), proto);
  caller->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::make_unique<DexCode>(), false);
  caller->get_code()->set_registers_size(1);

  auto invoke = new DexOpcodeMethod(OPCODE_INVOKE_STATIC, callee);
  invoke->set_arg_word_count(2);
  invoke->set_src(0, 0);
  invoke->set_src(1, 0);
  {
    MethodTransformer mtcaller(caller);
    mtcaller->push_back(invoke);
    mtcaller->push_back(dasm(OPCODE_RETURN_VOID));

    MethodTransformer mtcallee(callee);
    mtcallee->push_back(dasm(OPCODE_CONST_4, {1_v, 1_L}));
    mtcallee->push_back(dasm(OPCODE_RETURN_VOID));
  }
  {
    caller->get_code()->balloon();
    callee->get_code()->balloon();
    InlineContext inline_context(caller, /* use_liveness */ true);
    EXPECT_FALSE(MethodTransform::inline_16regs(inline_context, callee, invoke));
  }
  delete g_redex;
}
