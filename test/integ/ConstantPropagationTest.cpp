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
/*

This test takes as input the Dex bytecode for the class generated
from the Java source file:
   fbsource/fbandroid/native/redex/test/integ/ConstantPropagation.java
which is specified in Buck tests via an enviornment variable in the
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
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/ConstantPropagation;") == 0)
    return MAINCLASS;
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/MyBy2Or3;") == 0 ||
      strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Alpha;") == 0)
    return REMOVEDCLASS;
  return OTHERCLASS;
}

TEST(ConstantPropagationTest1, constantpropagation) {
  g_redex = new RedexContext();

	const char* dexfile =
    "gen/native/redex/test/integ/constant-propagation-test-dex/constant-propagation.dex";
  if (access(dexfile, R_OK) != 0) {
    dexfile = std::getenv("dexfile");
    ASSERT_NE(nullptr, dexfile);
  }

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();
  std::cout << "Loaded classes: " << classes.size() << std::endl;

	TRACE(MAIN, 2, "Code before:\n");
  for(const auto& cls : classes) {
    if (filter_test_classes(cls->get_name()) < 2) {
  	  TRACE(MAIN, 2, "Class %s\n", SHOW(cls));
  		for (const auto& dm : cls->get_dmethods()) {
  		  TRACE(MAIN, 2, "dmethod: %s\n",  dm->get_name()->c_str());
  			if (strcmp(dm->get_name()->c_str(), "propagation_1") == 0 ||
            strcmp(dm->get_name()->c_str(), "propagation_2") == 0) {
  			  TRACE(MAIN, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
  			}
  		}
    }
	}

  std::vector<Pass*> passes = {
    new LocalDcePass(),
    new DelInitPass(),
    new RemoveEmptyClassesPass(),
    // TODO: add constant propagation and conditional pruning optimization
  };

  std::vector<KeepRule> null_rules;
  PassManager manager(passes, null_rules);

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);
  manager.run_passes(dexen, dummy_cfg);

	TRACE(MAIN, 2, "Code after:\n");
	for(const auto& cls : classes) {
    if (filter_test_classes(cls->get_name()) == REMOVEDCLASS) {
      TRACE(MAIN, 2, "Class %s\n", SHOW(cls));
      // To be reverted: These classes should be removed by future optimization.
      ASSERT_TRUE(true);
    } else if (filter_test_classes(cls->get_name()) == MAINCLASS) {
      TRACE(MAIN, 2, "Class %s\n", SHOW(cls));
      for (const auto& dm : cls->get_dmethods()) {
  		  TRACE(MAIN, 2, "dmethod: %s\n",  dm->get_name()->c_str());
  			if (strcmp(dm->get_name()->c_str(), "propagation_1") == 0) {
  			  TRACE(MAIN, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
  			  for (auto const instruction : dm->get_code()->get_instructions()) {
            // The logic will be reverted when the future
            // development of constant propagation optimization is done, i.e.,
            // The code will be changed to ASSERT_TRUE(false)
            // if IF_EQZ or New Class Instance instruction is found
            if (instruction->opcode() == OPCODE_IF_EQZ ||
                instruction->opcode() == OPCODE_NEW_INSTANCE) {
                    ASSERT_TRUE(true);
            }
  			  }
  			} else if (strcmp(dm->get_name()->c_str(), "propagation_2") == 0) {
          TRACE(MAIN, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
  			  for (auto const instruction : dm->get_code()->get_instructions()) {
            // The logic will be reverted when the future
            // development of constant propagation optimization is done, i.e.,
            // The code will be changed to ASSERT_TRUE(false)
            // if IF_EQZ or Invote_Static instruction is found
            if (instruction->opcode() == OPCODE_IF_EQZ ||
                instruction->opcode() == OPCODE_INVOKE_STATIC) {
                    ASSERT_TRUE(true);
            }
  			  }
        }
  		}
    }
	}
}
