/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "Transform.h"

#include "DedupBlocksPass.h"

int count_someFunc_calls(cfg::ControlFlowGraph& cfg) {
  int num_some_func_calls = 0;
  for (auto& mie : InstructionIterable(cfg)) {
    TRACE(DEDUP_BLOCKS, 1, "%s\n", SHOW(mie.insn));
    if (mie.insn->has_method()) {
      DexMethodRef* called = mie.insn->get_method();
      if (strcmp(called->get_name()->c_str(), "someFunc") == 0) {
        num_some_func_calls++;
      }
    }
  }
  return num_some_func_calls;
}

TEST(DedupBlocksTest, useSwitch) {
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

  bool code_checked_before = false;
  TRACE(DEDUP_BLOCKS, 1, "Code before:\n");
  for (const auto& cls : classes) {
    TRACE(DEDUP_BLOCKS, 1, "Class %s\n", SHOW(cls));
    for (const auto& m : cls->get_vmethods()) {
      if (strcmp(m->get_name()->c_str(), "useSwitch") == 0) {
        code_checked_before = true;
        IRCode* code = m->get_code();
        code->build_cfg(true);
        EXPECT_EQ(count_someFunc_calls(code->cfg()), 3);
        code->clear_cfg();
      }
    }
  }
  EXPECT_TRUE(code_checked_before);

  std::vector<Pass*> passes = {
      new DedupBlocksPass(),
  };

  PassManager manager(passes);
  manager.set_testing_mode();

  Json::Value conf_obj = Json::nullValue;
  Scope external_classes;
  ConfigFiles dummy_cfg(conf_obj);
  manager.run_passes(stores, external_classes, dummy_cfg);

  bool code_checked_after = false;
  TRACE(DEDUP_BLOCKS, 1, "Code after:\n");
  for (const auto& cls : classes) {
    for (const auto& m : cls->get_vmethods()) {
      if (strcmp(m->get_name()->c_str(), "useSwitch") == 0) {
        code_checked_after = true;
        IRCode* code = m->get_code();
        code->build_cfg(true);
        EXPECT_EQ(count_someFunc_calls(code->cfg()), 1);
        code->clear_cfg();
      }
    }
  }
  EXPECT_TRUE(code_checked_after);
}
