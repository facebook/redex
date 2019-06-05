/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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

#include "CopyPropagationPass.h"

int count_sgets(cfg::ControlFlowGraph& cfg) {
  int sgets = 0;
  for (auto& mie : InstructionIterable(cfg)) {
    TRACE(RME, 1, "%s", SHOW(mie.insn));
    if (is_sget(mie.insn->opcode())) {
      sgets++;
    }
  }
  return sgets;
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

  TRACE(RME, 1, "Code before:");
  for (const auto& cls : classes) {
    TRACE(RME, 1, "Class %s", SHOW(cls));
    for (const auto& m : cls->get_vmethods()) {
      TRACE(RME, 1, "\nmethod %s:", SHOW(m));
      IRCode* code = m->get_code();
      code->build_cfg(/* editable */ true);
      EXPECT_EQ(2, count_sgets(code->cfg()));
      code->clear_cfg();
    }
  }

  auto copy_prop = new CopyPropagationPass();
  copy_prop->m_config.static_finals = true;
  std::vector<Pass*> passes = {
    copy_prop
  };

  PassManager manager(passes);
  manager.set_testing_mode();

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);
  manager.run_passes(stores, dummy_cfg);

  TRACE(RME, 1, "Code after:");
  for (const auto& cls : classes) {
    for (const auto& m : cls->get_vmethods()) {
      TRACE(RME, 1, "\nmethod %s:", SHOW(m));
      IRCode* code = m->get_code();
      code->build_cfg(/* editable */ true);
      if (strcmp(m->get_name()->c_str(), "remove") == 0) {
        EXPECT_EQ(1, count_sgets(code->cfg()));
      } else {
        EXPECT_EQ(2, count_sgets(code->cfg()));
      }
      code->clear_cfg();
    }
  }
}
