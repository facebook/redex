/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"

std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
  return os << show(&to_show);
}

TEST(CreatorsTest, Alloc) {
  using namespace dex_asm;

  g_redex = new RedexContext();

  MethodCreator mc(DexType::make_type("Lfoo;"),
                   DexString::make_string("bar"),
                   DexProto::make_proto(
                       DexType::make_type("V"),
                       DexTypeList::make_type_list(
                           {DexType::make_type("I"), DexType::make_type("J")})),
                   ACC_PUBLIC);
  auto loc = mc.make_local(DexType::make_type("I"));
  mc.get_main_block()->load_const(loc, 123);
  auto method = mc.create();
  auto it = InstructionIterable(method->get_code()).begin();

  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM_OBJECT, {1_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM, {2_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM_WIDE, {3_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST_16, {0_v, 123_L}));

  delete g_redex;
}
