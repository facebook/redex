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

using namespace dex_asm;

std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
  return os << show(&to_show);
}

MethodCreator make_method_creator() {

  MethodCreator mc(DexType::make_type("Lfoo;"),
                   DexString::make_string("bar"),
                   DexProto::make_proto(get_void_type(),

                                        DexTypeList::make_type_list(
                                            {get_int_type(), get_long_type()})),
                   ACC_PUBLIC);
  return mc;
}

TEST(CreatorsTest, Alloc) {
  g_redex = new RedexContext();

  auto mc = make_method_creator();
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
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {0_v, 123_L}));

  delete g_redex;
}

TEST(MakeSwitch, MultiIndices) {
  g_redex = new RedexContext();
  auto mc = make_method_creator();
  auto idx_loc = mc.make_local(get_int_type());
  auto param_loc = mc.get_local(1);
  auto mb = mc.get_main_block();
  mb->load_const(idx_loc, 1);

  // build switch
  std::map<SwitchIndices, MethodBlock*> cases;
  SwitchIndices indices1 = {0, 1};
  cases[indices1] = nullptr;
  SwitchIndices indices2 = {2};
  cases[indices2] = nullptr;
  SwitchIndices indices3 = {3};
  cases[indices3] = nullptr;

  auto def_block = mb->switch_op(idx_loc, cases);
  def_block->init_loc(param_loc);

  for (auto it : cases) {
    auto idx = it.first;
    auto case_block = cases[idx];
    ASSERT_TRUE(idx.size());
    case_block->binop_lit16(
        OPCODE_ADD_INT_LIT16, param_loc, param_loc, *idx.begin());
  }

  auto method = mc.create();
  printf(" code: \n%s\n", SHOW(method->get_code()));

  auto it = InstructionIterable(method->get_code()).begin();
  EXPECT_EQ(*it++->insn, *dasm(IOPCODE_LOAD_PARAM_OBJECT, {1_v}));
  EXPECT_EQ(*it++->insn, *dasm(IOPCODE_LOAD_PARAM, {2_v}));
  EXPECT_EQ(*it++->insn, *dasm(IOPCODE_LOAD_PARAM_WIDE, {3_v}));
  EXPECT_EQ(*it++->insn, *dasm(OPCODE_CONST, {0_v, 1_L}));
  EXPECT_EQ(*it++->insn, *dasm(OPCODE_PACKED_SWITCH, {0_v}));

  EXPECT_EQ(*it++->insn, *dasm(OPCODE_CONST, {2_v, 0_L}));

  EXPECT_EQ(*it++->insn, *dasm(OPCODE_ADD_INT_LIT16, {2_v, 2_v, 0_L}));
  EXPECT_EQ(*it++->insn, *dasm(OPCODE_GOTO, {}));

  EXPECT_EQ(*it++->insn, *dasm(OPCODE_ADD_INT_LIT16, {2_v, 2_v, 2_L}));
  EXPECT_EQ(*it++->insn, *dasm(OPCODE_GOTO, {}));

  EXPECT_EQ(*it++->insn, *dasm(OPCODE_ADD_INT_LIT16, {2_v, 2_v, 3_L}));
  EXPECT_EQ(*it++->insn, *dasm(OPCODE_GOTO, {}));

  method->sync();
  auto dex_code = method->get_dex_code();
  printf(" dex_code: \n%s\n", SHOW(dex_code));

  for (auto insn : dex_code->get_instructions()) {
    printf(" dex insn: %s; OP: %s\n", SHOW(insn), SHOW(insn->opcode()));
    if (insn->opcode() != FOPCODE_PACKED_SWITCH) {
      continue;
    }

    DexOpcodeData* dex_data = static_cast<DexOpcodeData*>(insn);
    const uint16_t* data = dex_data->data();
    printf(" data size: %d\n", dex_data->size());
    EXPECT_EQ(dex_data->size(), 12); // (4 cases * 2) + 4
    EXPECT_EQ(*data, 4); // 4 cases
    uint32_t* data32 = (uint32_t*)&data[1];
    EXPECT_EQ(*data32++, 0); // initial index value
    EXPECT_EQ(*data32++, 5); // case 0
    EXPECT_EQ(*data32++, 5); // case 1
    EXPECT_EQ(*data32++, 8); // case 2
    EXPECT_EQ(*data32++, 11); // case 3
  }

  delete g_redex;
}
