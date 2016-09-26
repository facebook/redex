/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "Transform.h"

#include "LocalDce.h"
#include "DelInit.h"
#include "RemoveEmptyClasses.h"
#include "ConstantPropagation.h"

/*

This test takes as input the Dex bytecode for the class generated
from the Java source file:
   <redex root>/test/integ/ConstantPropagation.java
which is specified in Buck tests via an environment variable in the
BUCK file. Before optimization, the code for the propagate method is:

dmethod: propagation_1
dmethod: regs: 4, ins: 0, outs: 2
const/4 v1
if-eqz v1
new-instance Lcom/facebook/redextest/MyBy2Or3; v0
const/4 v3
invoke-direct public com.facebook.redextest.MyBy2Or3.<init>(I)V v0, v3
invoke-virtual public com.facebook.redextest.MyBy2Or3.Double()I v0
move-result v2
return v2
const/16 v2
goto

dmethod: propagation_2
dmethod: regs: 2, ins: 0, outs: 0
const/4 v0
if-eqz v0
const/16 v1
return v1
invoke-static public static com.facebook.redextest.Alpha.theAnswer()I
move-result v1
goto

dmethod: propagation_3
dmethod: regs: 2, ins: 0, outs: 0
invoke-static public static com.facebook.redextest.Gamma.getConfig()Z
move-result v1
if-eqz v1
const/16 v0
return v0
invoke-static public static com.facebook.redextest.Alpha.theAnswer()I
move-result v0
goto


After optimization with LocalDcePass, DelInitPass, RemoveEmptyClassesPass
and ConstantPropagationPass, the code should be:

dmethod: propagation_1
dmethod: regs: 2, ins: 0, outs: 0
const/16 v1
return v1

dmethod: propagation_2
dmethod: regs: 2, ins: 0, outs: 0
const/16 v1
return v1

dmethod: propagation_3
dmethod: regs: 1, ins: 0, outs: 0
const/16 v0
return v0

This test mainly checks whether the constant propagation is fired. It does
this by checking to make sure there are no OPCODE_IF_EQZ and OPCODE_NEW_INSTANCE
instructions in the optimized method.
*/

// The ClassType enum is used to classify and filter classes in test result
enum ClassType {
  MAINCLASS = 0,
  REMOVEDCLASS = 1,
  OTHERCLASS = 2,
};

ClassType filter_test_classes(const DexString *cls_name) {
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Propagation;") == 0)
    return MAINCLASS;
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Alpha;") == 0)
    return REMOVEDCLASS;
  return OTHERCLASS;
}

TEST(ConstantPropagationTest1, constantpropagation) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("dexfile");
  ASSERT_NE(nullptr, dexfile);

  std::vector<DexStore> stores;
  DexStore root_store("classes");
  root_store.add_classes(load_classes_from_dex(dexfile));
  DexClasses& classes = root_store.get_dexen().back();
  stores.emplace_back(std::move(root_store));
  std::cout << "Loaded classes: " << classes.size() << std::endl;

  TRACE(CONSTP, 2, "Code before:\n");
  for(const auto& cls : classes) {
    TRACE(CONSTP, 2, "Class %s\n", SHOW(cls));
    if (filter_test_classes(cls->get_name()) < 2) {
      TRACE(CONSTP, 2, "Class %s\n", SHOW(cls));
      for (const auto& dm : cls->get_dmethods()) {
        TRACE(CONSTP, 2, "dmethod: %s\n",  dm->get_name()->c_str());
        if (strcmp(dm->get_name()->c_str(), "propagation_1") == 0 ||
            strcmp(dm->get_name()->c_str(), "propagation_2") == 0 ||
            strcmp(dm->get_name()->c_str(), "propagation_3") == 0) {
          TRACE(CONSTP, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
        }
      }
    }
  }

  std::vector<Pass*> passes = {
    new ConstantPropagationPass(),
    new LocalDcePass(),
    new DelInitPass(),
    new RemoveEmptyClassesPass()
  };

  std::vector<KeepRule> null_rules;
  PassManager manager(passes, null_rules);

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);
  dummy_cfg.using_seeds = true;
  manager.run_passes(stores, dummy_cfg);

  TRACE(CONSTP, 2, "Code after:\n");
  for(const auto& cls : classes) {
    TRACE(CONSTP, 2, "Class %s\n", SHOW(cls));
    ASSERT_NE(filter_test_classes(cls->get_name()), REMOVEDCLASS);
    if (filter_test_classes(cls->get_name()) == MAINCLASS) {
      for (const auto& dm : cls->get_dmethods()) {
        TRACE(CONSTP, 2, "dmethod: %s\n",  dm->get_name()->c_str());
        if (strcmp(dm->get_name()->c_str(), "propagation_1") == 0) {
          TRACE(CONSTP, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
          for (auto const instruction : dm->get_code()->get_instructions()) {
            ASSERT_NE(instruction->opcode(), OPCODE_IF_EQZ);
            ASSERT_NE(instruction->opcode(), OPCODE_NEW_INSTANCE);
          }
        } else if (strcmp(dm->get_name()->c_str(), "propagation_2") == 0) {
          TRACE(CONSTP, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
          for (auto const instruction : dm->get_code()->get_instructions()) {
            ASSERT_NE(instruction->opcode(), OPCODE_IF_EQZ);
            ASSERT_NE(instruction->opcode(), OPCODE_INVOKE_STATIC);
          }
        } else if(strcmp(dm->get_name()->c_str(), "propagation_3") == 0) {
          TRACE(CONSTP, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
          for (auto const instruction : dm->get_code()->get_instructions()) {
            ASSERT_NE(instruction->opcode(), OPCODE_IF_EQZ);
            ASSERT_NE(instruction->opcode(), OPCODE_INVOKE_STATIC);
          }
        }
      }
    }
  }
}
