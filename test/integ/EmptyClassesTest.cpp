/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdlib>
#include <memory>
#include <gtest/gtest.h>
#include <string>

#include <json/json.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "PassManager.h"
#include "RedexContext.h"

#include "DelInit.h"
#include "RemoveEmptyClasses.h"

// NOTE: this is not really a unit test.

TEST(EmptyClassesTest1, emptyclasses) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("dexfile");
  ASSERT_NE(nullptr, dexfile);

  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);
  root_store.add_classes(load_classes_from_dex(dexfile));
  DexClasses& classes = root_store.get_dexen().back();
  stores.emplace_back(std::move(root_store));
  size_t before = classes.size();
  TRACE(EMPTY, 3, "Loaded classes: %ld\n", classes.size());
  // Report the classes that were loaded through tracing.
  for (const auto& cls : classes) {
    TRACE(EMPTY, 3, "Input class: %s\n",
        cls->get_type()->get_name()->c_str());
  }

  std::vector<Pass*> passes = {
    new DelInitPass(),
    new RemoveEmptyClassesPass(),
  };

  PassManager manager(passes);
  manager.set_testing_mode();

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);
  manager.run_passes(stores, dummy_cfg);

  size_t after = 0;
  std::set<std::string> remaining_classes;
  for (const auto& dex_classes : stores[0].get_dexen()) {
    for (const auto cls : dex_classes) {
      TRACE(EMPTY, 3, "Output class: %s\n",
          cls->get_type()->get_name()->c_str());
      after++;
      remaining_classes.insert(SHOW(cls->get_type()->get_name()));
    }
  }
  TRACE(EMPTY, 2, "Removed %ld classes\n", before - after);
  ASSERT_EQ(0, remaining_classes.count("Lcom/facebook/redextest/EmptyClasses;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/InnerEmpty;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/InnerEmpty$InnerClass;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/InnerEmpty2;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/InnerEmpty2$InnerClass2;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NotAnEmptyClass;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NotAnEmptyClass2;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NotAnEmptyClass3;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NotAnEmptyClass4;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NotAnEmptyClass5;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NotAnEmptyClass5;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/YesNo;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/MyYesNo;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/EasilyDone;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/By2Or3;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/MyBy2Or3;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/WombatException;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NumbatException;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/Wombat;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/EmptyButLaterExtended;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/Extender;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NotUsedHere;"));
  ASSERT_EQ(0, remaining_classes.count("Lcom/facebook/redextest/DontKillMeNow;"));

  delete g_redex;
}
