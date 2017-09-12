/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include <functional>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "PassManager.h"
#include "RedexContext.h"

#include "DelInit.h"
#include "LocalDce.h"
#include "RemoveEmptyClasses.h"
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

typedef std::function<void(const MethodItemEntry& mei)> MethodItemCallback;
typedef std::function<void(const DexClasses& classes)> DexClassesCallback;

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

void run_test_pass(Pass* pass, DexClassesCallback const& callback) {
  g_redex = new RedexContext();
  const char* dexfile = std::getenv("dexfile");
  EXPECT_NE(nullptr, dexfile);
  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);
  root_store.add_classes(load_classes_from_dex(dexfile));
  DexClasses& classes = root_store.get_dexen().back();
  stores.emplace_back(std::move(root_store));
  std::vector<Pass*> passes = {pass};
  PassManager manager(passes);
  manager.set_testing_mode();
  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);
  Scope external_classes;
  manager.run_passes(stores, external_classes, dummy_cfg);
  callback(classes);
}

void run_test_pass(Pass* pass, MethodItemCallback const& callback) {
  run_test_pass(pass, [&](const DexClasses& classes) {
    foreach_method_entry_item(classes, callback);
  });
}

} // anonymous namespace

TEST(StripDebugInfoTest, StripPrologueEnd) {
  // Test that we can remove all DBG_SET_PROLOGUE_END ops.
  auto pass = new StripDebugInfoPass();
  pass->set_drop_prologue_end(true);

  run_test_pass(pass, [&](const MethodItemEntry& mei) {
    if (mei.type == MFLOW_DEBUG) {
      DexDebugInstruction* dbgop = mei.dbgop.get();
      auto op = dbgop->opcode();
      EXPECT_NE(DBG_SET_PROLOGUE_END, op);
    }
  });
}

TEST(StripDebugInfoTest, StripEpilogueBegin) {
  // Test that we can remove all DBG_SET_EPILOGUE_BEGIN ops.
  auto pass = new StripDebugInfoPass();
  pass->set_drop_epilogue_begin(true);

  run_test_pass(pass, [&](const MethodItemEntry& mei) {
    if (mei.type == MFLOW_DEBUG) {
      DexDebugInstruction* dbgop = mei.dbgop.get();
      auto op = dbgop->opcode();
      EXPECT_NE(DBG_SET_EPILOGUE_BEGIN, op);
    }
  });
}

TEST(StripDebugInfoTest, StripLocals) {
  // Test that we can remove all DBG_*LOCAL* ops.
  auto pass = new StripDebugInfoPass();
  pass->set_drop_local_variables(true);

  run_test_pass(pass, [&](const MethodItemEntry& mei) {
    if (mei.type == MFLOW_DEBUG) {
      DexDebugInstruction* dbgop = mei.dbgop.get();
      auto op = dbgop->opcode();
      // Make sure all DBG_*LOCAL* ops were removed.
      EXPECT_NE(DBG_START_LOCAL, op);
      EXPECT_NE(DBG_START_LOCAL_EXTENDED, op);
      EXPECT_NE(DBG_END_LOCAL, op);
      EXPECT_NE(DBG_RESTART_LOCAL, op);
    }
  });
}

TEST(StripDebugInfoTest, StripAllDebugInfo) {
  // Test that we can remove all debug info.
  auto pass = new StripDebugInfoPass();
  pass->set_drop_all_debug_info(true);

  run_test_pass(pass, [&](const MethodItemEntry& mei) {
    EXPECT_NE(MFLOW_DEBUG, mei.type);
    EXPECT_NE(MFLOW_POSITION, mei.type);
  });
}

TEST(StripDebugInfoTest, StripAllLineNumbers) {
  // Test that we can remove all line number information.
  auto pass = new StripDebugInfoPass();
  pass->set_drop_line_numbers(true);

  run_test_pass(pass, [&](const MethodItemEntry& mei) {
    EXPECT_NE(MFLOW_POSITION, mei.type);
  });
}
