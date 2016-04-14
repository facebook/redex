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

#include <folly/json.h>
#include <folly/dynamic.h>

#include "DexClass.h"
#include "DexOpcode.h"
#include "DexLoader.h"
#include "PassManager.h"
#include "RedexContext.h"

#include "DelInit.h"
#include "RemoveEmptyClasses.h"

// NOTE: this is not really a unit test.

TEST(EmptyClassesTest1, emptyclasses) {
  g_redex = new RedexContext();

  const char* dexfile = "empty-classes-test-class.dex";
  if (access(dexfile, R_OK) != 0) {
    dexfile = std::getenv("dexfile");
    ASSERT_NE(nullptr, dexfile);
  }

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();
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

  std::vector<KeepRule> null_rules;
  auto const keep = { "Lcom/facebook/redextest/DoNotStrip;" };
  PassManager manager(
    passes,
    null_rules,
    folly::dynamic::object(
      "keep_annotations", folly::dynamic(keep.begin(), keep.end()))
  );
  manager.run_passes(dexen);

  size_t after = 0;
  std::set<std::string> remaining_classes;
  for (const auto& dex_classes : dexen) {
    for (const auto cls : dex_classes) {
      TRACE(EMPTY, 3, "Output class: %s\n",
          cls->get_type()->get_name()->c_str());
      after++;
      remaining_classes.insert(SHOW(cls->get_type()->get_name()));
    }
  }
  TRACE(EMPTY, 2, "Removed %ld classes\n", before - after);
  ASSERT_EQ(0, remaining_classes.count("Lcom/facebook/redextest/EmptyClasses;"));
  ASSERT_EQ(0, remaining_classes.count("Lcom/facebook/redextest/InnerEmpty;"));
  ASSERT_EQ(0, remaining_classes.count("Lcom/facebook/redextest/InnerEmpty$InnerClass;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/InnerEmpty2;"));
  ASSERT_EQ(0, remaining_classes.count("Lcom/facebook/redextest/InnerEmpty2$InnerClass2;"));
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
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/Wombat;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/EmptyButLaterExtended;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/Extender;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/NotUsedHere;"));
  ASSERT_EQ(1, remaining_classes.count("Lcom/facebook/redextest/DontKillMeNow;"));

  delete g_redex;
}
