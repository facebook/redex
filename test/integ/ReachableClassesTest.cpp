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
#include <string>

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "RedexContext.h"

TEST(ReachableClasses, ClassForNameStringLiteral) {
  g_redex = new RedexContext();

  // Hardcoded path is for OSS automake test harness, environment variable is
  // for Buck
  const char* dexfile = "reachable-classes.dex";
  ASSERT_NE(nullptr, dexfile);
  if (access(dexfile, R_OK) != 0) {
    dexfile = std::getenv("dexfile");
    ASSERT_NE(nullptr, dexfile);
  }

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

  std::vector<Pass*> passes;
  Json::Value conf_obj;

  PassManager manager(
    passes,
    conf_obj
  );
  manager.set_testing_mode();

  ConfigFiles dummy_cfg(conf_obj);
  Scope external_classes;
  manager.run_passes(stores, external_classes, dummy_cfg);

  auto type1 = type_class(DexType::get_type("Lcom/facebook/redextest/Type1;"));
  auto type2 = type_class(DexType::get_type("Lcom/facebook/redextest/Type2;"));
  auto type3 = type_class(DexType::get_type("Lcom/facebook/redextest/Type3;"));
  auto type4 = type_class(DexType::get_type("Lcom/facebook/redextest/Type3$Type4;"));
  auto type5 = type_class(DexType::get_type("Lcom/facebook/redextest/Type5;"));

  ASSERT_TRUE(can_rename(type1));
  ASSERT_FALSE(can_rename(type2));
  ASSERT_FALSE(can_rename(type3));
  ASSERT_FALSE(can_rename(type4));
  ASSERT_TRUE(can_rename(type5));

  delete g_redex;
}
