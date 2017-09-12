/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdlib>
#include <gtest/gtest.h>
#include <json/json.h>
#include <memory>
#include <string>

#include "VerifyUtil.h"

TEST_F(PreVerify, StripDebugInfo) {
  bool found_prologue_end = false;
  for (const auto& cls : classes) {
    std::vector<DexMethodRef*> methods;
    cls->gather_methods(methods);
    for (auto dm : methods) {
      if (!dm->is_def()) continue;
      auto code = static_cast<DexMethod*>(dm)->get_dex_code();
      if (!code) continue;
      auto debug_item = code->get_debug_item();
      if (!debug_item) continue;
      for (const auto& e : debug_item->get_entries()) {
        if (e.type == DexDebugEntryType::Instruction) {
          DexDebugInstruction* dbg_op = e.insn.get();
          if (!dbg_op) continue;
          auto op = dbg_op->opcode();
          if (op == DBG_SET_PROLOGUE_END) {
            found_prologue_end = true;
            break;
          }
        }
      }
      if (found_prologue_end) break;
    }
  }
  EXPECT_TRUE(found_prologue_end);
}

TEST_F(PostVerify, StripDebugInfo) {
  for (const auto& cls : classes) {
    std::vector<DexMethodRef*> methods;
    cls->gather_methods(methods);
    for (auto dm : methods) {
      if (!dm->is_def()) continue;
      auto code = static_cast<DexMethod*>(dm)->get_dex_code();
      if (!code) {
        continue;
      }
      auto debug_item = code->get_debug_item();
      if (!debug_item) {
        continue;
      }
      for (const auto& e : debug_item->get_entries()) {
        if (e.type == DexDebugEntryType::Instruction) {
          DexDebugInstruction* dbg_op = e.insn.get();
          if (!dbg_op) continue;
          auto op = dbg_op->opcode();
          // Make sure all prologue begin, epilogue end and local variable
          // references are gone.
          EXPECT_NE(DBG_SET_PROLOGUE_END, op);
          EXPECT_NE(DBG_SET_EPILOGUE_BEGIN, op);
          EXPECT_NE(DBG_START_LOCAL, op);
          EXPECT_NE(DBG_START_LOCAL_EXTENDED, op);
          EXPECT_NE(DBG_END_LOCAL, op);
          EXPECT_NE(DBG_RESTART_LOCAL, op);
        }
      }
    }
  }
}
