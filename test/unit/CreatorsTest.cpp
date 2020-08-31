/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "Show.h"

using namespace dex_asm;

MethodCreator make_method_creator() {

  MethodCreator mc(DexType::make_type("Lfoo;"),
                   DexString::make_string("bar"),
                   DexProto::make_proto(type::_void(),

                                        DexTypeList::make_type_list(
                                            {type::_int(), type::_long()})),
                   ACC_PUBLIC);
  return mc;
}

class CreatorsTest : public RedexTest {};

TEST_F(CreatorsTest, Alloc) {
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
}

TEST_F(CreatorsTest, MakeSwitchMultiIndices) {
  auto mc = make_method_creator();
  auto idx_loc = mc.make_local(type::_int());
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

  for (const auto& it : cases) {
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
  EXPECT_EQ(*it++->insn, *dasm(OPCODE_SWITCH, {0_v}));

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
}

TEST_F(CreatorsTest, ClassCreator) {
  std::string foo("Lfoo;");
  ClassCreator cc(DexType::make_type(foo.c_str()));
  cc.set_super(type::java_lang_Object());
  auto cls = cc.create();
  std::string bar("Lbar;");
  cls->set_deobfuscated_name(bar);

  auto foo_type = DexType::get_type(foo);
  auto bar_type = DexType::get_type(bar);
  EXPECT_EQ(foo_type, cls->get_type());
  EXPECT_EQ(bar_type, cls->get_type());
}
