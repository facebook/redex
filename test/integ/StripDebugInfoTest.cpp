/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <functional>

#include "DexInstruction.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RedexTest.h"

#include "LocalDce.h"
#include "StripDebugInfo.h"

/*

This test takes as input the Dex bytecode for the class generated
from the Java source file:
   <redex root>/test/integ/StripDebugInfo.java
which is specified in Buck tests via an environment variable in the
BUCK file. This test will test removing debug info using the
StripDebugInfo pass and setting variables up differently for each
pass.
 */

namespace {

using MethodItemCallback = std::function<void(const MethodItemEntry&)>;
using DexClassesCallback = std::function<void(const DexClasses&)>;

} // anonymous namespace

class StripDebugInfoTest : public RedexIntegrationTest {
 protected:
  void foreach_method_entry_item(const DexClasses& classes,
                                 MethodItemCallback const& callback) {
    for (const auto& cls : classes) {
      std::vector<DexMethodRef*> methods;
      cls->gather_methods(methods);
      for (auto method : methods) {
        if (!method->is_def()) continue;
        IRCode* code = static_cast<DexMethod*>(method)->get_code();
        if (!code) continue;
        for (const auto& mei : *code)
          callback(mei);
      }
    }
  }

  template <typename Fn>
  void run_test_pass(Pass& pass,
                     DexClassesCallback const& callback,
                     const Fn& fn) {
    std::vector<Pass*> passes = {&pass};
    run_passes(passes, nullptr, Json::nullValue, fn);
    callback(*classes);
  }

  template <typename Fn>
  void run_test_pass(Pass& pass,
                     MethodItemCallback const& callback,
                     const Fn& fn) {
    run_test_pass(
        pass,
        [&](const DexClasses& classes) {
          foreach_method_entry_item(classes, callback);
        },
        fn);
  }
};

TEST_F(StripDebugInfoTest, StripPrologueEnd) {
  // Test that we can remove all DBG_SET_PROLOGUE_END ops.
  auto pass = StripDebugInfoPass();

  run_test_pass(
      pass,
      [&](const MethodItemEntry& mei) {
        if (mei.type == MFLOW_DEBUG) {
          DexDebugInstruction* dbgop = mei.dbgop.get();
          auto op = dbgop->opcode();
          EXPECT_NE(DBG_SET_PROLOGUE_END, op);
        }
      },
      [&](const auto&) { pass.set_drop_prologue_end(true); });
}

TEST_F(StripDebugInfoTest, StripEpilogueBegin) {
  // Test that we can remove all DBG_SET_EPILOGUE_BEGIN ops.
  auto pass = StripDebugInfoPass();

  run_test_pass(
      pass,
      [&](const MethodItemEntry& mei) {
        if (mei.type == MFLOW_DEBUG) {
          DexDebugInstruction* dbgop = mei.dbgop.get();
          auto op = dbgop->opcode();
          EXPECT_NE(DBG_SET_EPILOGUE_BEGIN, op);
        }
      },
      [&](const auto&) { pass.set_drop_epilogue_begin(true); });
}

TEST_F(StripDebugInfoTest, StripLocals) {
  // Test that we can remove all DBG_*LOCAL* ops.
  auto pass = StripDebugInfoPass();

  run_test_pass(
      pass,
      [&](const MethodItemEntry& mei) {
        if (mei.type == MFLOW_DEBUG) {
          DexDebugInstruction* dbgop = mei.dbgop.get();
          auto op = dbgop->opcode();
          // Make sure all DBG_*LOCAL* ops were removed.
          EXPECT_NE(DBG_START_LOCAL, op);
          EXPECT_NE(DBG_START_LOCAL_EXTENDED, op);
          EXPECT_NE(DBG_END_LOCAL, op);
          EXPECT_NE(DBG_RESTART_LOCAL, op);
        }
      },
      [&](const auto&) { pass.set_drop_local_variables(true); });
}

TEST_F(StripDebugInfoTest, StripAllDebugInfo) {
  // Test that we can remove all debug info.
  auto pass = StripDebugInfoPass();

  run_test_pass(
      pass,
      [&](const MethodItemEntry& mei) {
        EXPECT_NE(MFLOW_DEBUG, mei.type);
        EXPECT_NE(MFLOW_POSITION, mei.type);
      },
      [&](const auto&) { pass.set_drop_all_debug_info(true); });
}

TEST_F(StripDebugInfoTest, StripAllLineNumbers) {
  // Test that we can remove all line number information.
  auto pass = StripDebugInfoPass();

  run_test_pass(
      pass,
      [&](const MethodItemEntry& mei) { EXPECT_NE(MFLOW_POSITION, mei.type); },
      [&](const auto&) { pass.set_drop_line_numbers(true); });
}
