/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ApkResources.h"
#include "BundleResources.h"
#include "RedexResources.h"
#include "ResourcesInliningPass.h"
#include "verify/VerifyUtil.h"

void resource_inlining_PreVerify(ResourceTableFile* res_table) {
  EXPECT_THAT(res_table->sorted_res_ids,
              testing::ElementsAre(
                  /* integer array */
                  0x7f010000,
                  /* bool */
                  0x7f020000,
                  /* color */
                  0x7f030000,
                  0x7f030001,
                  0x7f030002,
                  0x7f030003,
                  0x7f030004,
                  0x7f030005,
                  0x7f030006,
                  0x7f030007,
                  /* dimen */
                  0x7f040000,
                  0x7f040001,
                  /* integer */
                  0x7f050000,
                  /* layout */
                  0x7f060000,
                  0x7f060001,
                  0x7f060002,
                  /* string */
                  0x7f070000,
                  0x7f070001,
                  0x7f070002));
  std::unordered_map<uint32_t, resources::InlinableValue> inlinable_pre_filter =
      res_table->get_inlinable_resource_values();
  std::unordered_set<std::string> resource_type_names = {"bool", "color",
                                                         "integer"};
  std::unordered_set<std::string> resource_entry_names = {"string/main_text"};
  auto inlinable =
      ResourcesInliningPass::filter_inlinable_resources(res_table,
                                                        inlinable_pre_filter,
                                                        resource_type_names,
                                                        resource_entry_names);
  EXPECT_TRUE(inlinable.find(0x7f010000) == inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f020000) != inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f030000) != inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f030001) != inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f030002) != inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f030003) != inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f030004) == inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f030005) == inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f030006) == inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f030007) == inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f040000) == inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f040001) == inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f050000) != inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f060000) == inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f060001) == inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f060002) == inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f070000) != inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f070001) == inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f070002) == inlinable.end());

  auto val = inlinable.at(0x7f020000);
  EXPECT_EQ(val.type, android::Res_value::TYPE_INT_BOOLEAN);
  EXPECT_TRUE(val.bool_value);

  val = inlinable.at(0x7f030000);
  EXPECT_EQ(val.type, android::Res_value::TYPE_INT_COLOR_RGB8);
  EXPECT_EQ(val.uint_value, 0xff673ab7);

  val = inlinable.at(0x7f030001);
  EXPECT_GE(val.type, android::Res_value::TYPE_FIRST_COLOR_INT);
  EXPECT_LE(val.type, android::Res_value::TYPE_LAST_COLOR_INT);
  EXPECT_EQ(val.uint_value, 0xffff0000);

  val = inlinable.at(0x7f030002);
  EXPECT_EQ(val.type, android::Res_value::TYPE_INT_COLOR_RGB8);
  EXPECT_EQ(val.uint_value, 0xff673ab7);

  val = inlinable.at(0x7f030003);
  EXPECT_EQ(val.type, android::Res_value::TYPE_INT_COLOR_RGB8);
  EXPECT_EQ(val.uint_value, 0xff673ab7);

  val = inlinable.at(0x7f050000);
  EXPECT_GE(val.type, android::Res_value::TYPE_FIRST_INT);
  EXPECT_LE(val.type, android::Res_value::TYPE_INT_HEX);
  EXPECT_EQ(val.uint_value, 3);

  val = inlinable.at(0x7f070000);
  EXPECT_EQ(val.type, android::Res_value::TYPE_STRING);
  EXPECT_EQ(val.string_value.substr(0, 6), "Hello,");
}

void resource_inlining_PostVerify(DexClass* cls) {
  auto method = find_method_named(*cls, "logValues");
  IRCode* code = new IRCode(method);
  code->build_cfg();
  auto& cfg = code->cfg();
  for (auto* block : cfg.blocks()) {
    int line_num = 0;
    for (auto& mie : InstructionIterable(block)) {
      line_num++;
      auto insn = mie.insn;
      if (block->id() == 0) {
        if (line_num == 6) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(insn->get_literal(), 1);
        }
      } else if (block->id() == 1) {
        if (line_num == 3) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(uint32_t(insn->get_literal()), 0xFFFF0000);
        } else if (line_num == 20) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(uint32_t(insn->get_literal()), 0xFF673AB7);
        } else if (line_num == 49) {
          ASSERT_EQ(insn->opcode(), OPCODE_SGET);
        } else if (line_num == 50) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO);
        } else if (line_num == 51) {
          ASSERT_EQ(insn->opcode(), OPCODE_SGET);
        } else if (line_num == 52) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO);
        } else if (line_num == 53) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(uint32_t(insn->get_literal()), 3);
        } else if (line_num == 56) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string_char_star = insn->get_string()->c_str();
          auto string = (std::string)string_char_star;
          ASSERT_EQ(string.substr(0, 6), "Hello,");
        } else if (line_num == 57) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        }
      } else if (block->id() == 4) {
        if (line_num == 31) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(uint32_t(insn->get_literal()), 0xFFFFFFFF);
        } else if (line_num == 46) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string_char_star = insn->get_string()->c_str();
          auto string = (std::string)string_char_star;
          ASSERT_EQ(string, "#ff673ab7");
        } else if (line_num == 47) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        } else if (line_num == 60) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string_char_star = insn->get_string()->c_str();
          auto string = (std::string)string_char_star;
          ASSERT_EQ(string, "3");
        } else if (line_num == 61) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        } else if (line_num == 74) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string_char_star = insn->get_string()->c_str();
          auto string = (std::string)string_char_star;
          ASSERT_EQ(string, "com.fb.resources:integer/loop_count");
        } else if (line_num == 75) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        } else if (line_num == 88) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string_char_star = insn->get_string()->c_str();
          auto string = (std::string)string_char_star;
          ASSERT_EQ(string, "loop_count");
        } else if (line_num == 89) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        }
      }
    }
  }
}
