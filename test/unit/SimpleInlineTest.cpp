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
  auto callee = DexMethod::make_method("Lfoo;", "testCallee", "V", {"I", "I"});
  callee->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  callee->set_code(std::make_unique<IRCode>(callee, 0));

  auto caller = DexMethod::make_method("Lfoo;", "testCaller", "V", {"I", "I"});
  caller->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  caller->set_code(std::make_unique<IRCode>(caller, 0));

  auto invoke = dasm(OPCODE_INVOKE_STATIC, callee, {});
  invoke->set_arg_word_count(2);
  invoke->set_src(0, 0);
  invoke->set_src(1, 0);

  auto mtcaller = caller->get_code();
  mtcaller->push_back(invoke);
  mtcaller->push_back(dasm(OPCODE_RETURN_VOID));

  auto mtcallee = callee->get_code();
  mtcallee->push_back(dasm(OPCODE_CONST_4, {1_v, 1_L}));
  mtcallee->push_back(dasm(OPCODE_RETURN_VOID));

  InlineContext inline_context(caller, /* use_liveness */ true);
  EXPECT_FALSE(IRCode::inline_method(
      inline_context, callee, invoke, /* no_exceed_16regs */ true));
  delete g_redex;
}
