/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "IRInstruction.h"
#include "Match.h"
#include "RedexTest.h"
#include "Show.h"

class MatchTest : public RedexTest {};

TEST_F(MatchTest, IputBasic) {
  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexFieldRef* field = DexField::make_field(ty, str, ty);

  auto iput = std::make_unique<IRInstruction>(OPCODE_IPUT);
  iput->set_src(0, 0);
  iput->set_src(1, 1);
  iput->set_field(field);

  std::vector<IRInstruction*> input{iput.get()};
  std::vector<IRInstruction*> match;

  m::find_insn_match(input, m::iput(), match);
  ASSERT_EQ(match.size(), 1);
  EXPECT_EQ(match[0], iput.get());
}

TEST_F(MatchTest, IgetBasic) {
  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexFieldRef* field = DexField::make_field(ty, str, ty);

  auto iget = std::make_unique<IRInstruction>(OPCODE_IGET);
  iget->set_src(0, 0);
  iget->set_field(field);

  std::vector<IRInstruction*> input{iget.get()};
  std::vector<IRInstruction*> match;

  m::find_insn_match(input, m::iget(), match);
  ASSERT_EQ(match.size(), 1);
  EXPECT_EQ(match[0], iget.get());
}

TEST_F(MatchTest, InvokeBasic) {
  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexProto* proto = DexProto::make_proto(ty, DexTypeList::make_type_list({}));
  DexMethodRef* method = DexMethod::make_method(ty, str, proto);

  auto invoke = std::make_unique<IRInstruction>(OPCODE_INVOKE_VIRTUAL);
  invoke->set_method(method);

  std::vector<IRInstruction*> input{invoke.get()};
  std::vector<IRInstruction*> match;

  m::find_insn_match(input, m::invoke(), match);
  ASSERT_EQ(match.size(), 1);
  EXPECT_EQ(match[0], invoke.get());
}

TEST_F(MatchTest, opcode_string) {
  DexString* str = DexString::make_string("foo");

  IRInstruction* load_str = new IRInstruction(OPCODE_CONST_STRING);
  load_str->set_string(str);

  std::vector<IRInstruction*> input = {load_str};
  std::vector<IRInstruction*> match;

  m::find_insn_match(input, m::opcode_string(m::ptr_eq(str)), match);
  EXPECT_EQ(match[0], load_str);
  delete load_str;
}

TEST_F(MatchTest, NotAllMatch) {
  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexFieldRef* field = DexField::make_field(ty, str, ty);

  auto iput = std::make_unique<IRInstruction>(OPCODE_IPUT);
  iput->set_src(0, 0);
  iput->set_src(1, 1);
  iput->set_field(field);

  auto iget = std::make_unique<IRInstruction>(OPCODE_IGET);
  iget->set_src(0, 0);
  iget->set_field(field);

  DexProto* proto = DexProto::make_proto(ty, DexTypeList::make_type_list({}));
  DexMethodRef* method = DexMethod::make_method(ty, str, proto);

  auto invoke = std::make_unique<IRInstruction>(OPCODE_INVOKE_VIRTUAL);
  invoke->set_method(method);

  std::vector<IRInstruction*> input{iget.get(), iput.get(), invoke.get()};
  std::vector<IRInstruction*> match;

  m::find_insn_match(input, m::iput(), match);
  ASSERT_EQ(match.size(), 1);
  EXPECT_EQ(match[0], iput.get());
}

TEST_F(MatchTest, SameFieldMatch) {
  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexFieldRef* field = DexField::make_field(ty, str, ty);

  auto iput = std::make_unique<IRInstruction>(OPCODE_IPUT);
  iput->set_src(0, 0);
  iput->set_src(1, 1);
  iput->set_field(field);

  auto iget = std::make_unique<IRInstruction>(OPCODE_IGET);
  iget->set_src(0, 0);
  iget->set_field(field);

  DexProto* proto = DexProto::make_proto(ty, DexTypeList::make_type_list({}));
  DexMethodRef* method = DexMethod::make_method(ty, str, proto);

  auto invoke = std::make_unique<IRInstruction>(OPCODE_INVOKE_VIRTUAL);
  invoke->set_method(method);

  std::vector<IRInstruction*> input{iget.get(), iput.get(), invoke.get()};
  std::vector<IRInstruction*> match;

  m::find_insn_match(
      input, m::opcode_field(m::member_of<DexFieldRef>(m::ptr_eq(ty))), match);
  EXPECT_EQ(match.size(), 2);
}

TEST_F(MatchTest, SameMethodMatch) {
  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexFieldRef* field = DexField::make_field(ty, str, ty);

  auto iput = std::make_unique<IRInstruction>(OPCODE_IPUT);
  iput->set_src(0, 0);
  iput->set_src(1, 1);
  iput->set_field(field);

  auto iget = std::make_unique<IRInstruction>(OPCODE_IGET);
  iget->set_src(0, 0);
  iget->set_field(field);

  DexProto* proto = DexProto::make_proto(ty, DexTypeList::make_type_list({}));
  DexMethodRef* method = DexMethod::make_method(ty, str, proto);

  auto invoke = std::make_unique<IRInstruction>(OPCODE_INVOKE_VIRTUAL);
  invoke->set_method(method);

  std::vector<IRInstruction*> input = {iget.get(), iput.get(), invoke.get()};
  std::vector<IRInstruction*> match;

  m::find_insn_match(
      input, m::opcode_method(m::member_of<DexMethodRef>(m::ptr_eq(ty))),
      match);
  EXPECT_EQ(match.size(), 1);
}
