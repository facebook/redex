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
#include "DexUtil.h"
#include "IRCode.h"
#include "PassManager.h"
#include "RedexContext.h"

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

dmethod: if_false
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

dmethod: if_true
dmethod: regs: 2, ins: 0, outs: 0
const/4 v0
if-eqz v0
const/16 v1
return v1
invoke-static public static com.facebook.redextest.Alpha.theAnswer()I
move-result v1
goto


After optimization with ConstantPropagationPass, the code should be:

dmethod: if_false
dmethod: regs: 2, ins: 0, outs: 0
const/16 v1
return v1

dmethod: if_true
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
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Propagation;") == 0)
    return MAINCLASS;
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Alpha;") == 0)
    return REMOVEDCLASS;
  return OTHERCLASS;
}

TEST(ConstantPropagationTest, constantPropagation) {
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
  std::cout << "Loaded classes: " << classes.size() << std::endl;

  TRACE(CONSTP, 1, "Code before:\n");
  for(const auto& cls : classes) {
    TRACE(CONSTP, 1, "Class %s\n", SHOW(cls));
    if (filter_test_classes(cls->get_name()) < 2) {
      TRACE(CONSTP, 1, "Class %s\n", SHOW(cls));
      for (const auto& dm : cls->get_dmethods()) {
        TRACE(CONSTP, 1, "dmethod: %s\n",  dm->get_name()->c_str());
        if (strcmp(dm->get_name()->c_str(), "if_false") == 0 ||
            strcmp(dm->get_name()->c_str(), "if_true") == 0 ||
            strcmp(dm->get_name()->c_str(), "if_unknown") == 0) {
          TRACE(CONSTP, 1, "%s\n", SHOW(InstructionIterable(dm->get_code())));
        }
      }
    }
  }

  Pass* constp = nullptr;
  constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {
    constp
  };

  PassManager manager(passes);
  manager.set_testing_mode();

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);
  Scope external_classes;
  manager.run_passes(stores, external_classes, dummy_cfg);

  TRACE(CONSTP, 1, "Code after:\n");
  for(const auto& cls : classes) {
    TRACE(CONSTP, 1, "Class %s\n", SHOW(cls));
    if (filter_test_classes(cls->get_name()) == MAINCLASS) {
      for (const auto& dm : cls->get_dmethods()) {
        TRACE(CONSTP, 1, "dmethod: %s\n",  dm->get_name()->c_str());
        if (strcmp(dm->get_name()->c_str(), "if_false") == 0) {
          const auto& insns = InstructionIterable(dm->get_code());
          TRACE(CONSTP, 1, "%s\n", SHOW(insns));
          for (auto& mie : insns) {
            IROpcode op = mie.insn->opcode();
            EXPECT_NE(op, OPCODE_IF_EQZ);
          }
        } else if (strcmp(dm->get_name()->c_str(), "if_true") == 0) {
          const auto& insns = InstructionIterable(dm->get_code());
          TRACE(CONSTP, 1, "%s\n", SHOW(insns));
          for (auto& mie : insns) {
            IROpcode op = mie.insn->opcode();
            EXPECT_NE(op, OPCODE_IF_EQZ);
          }
        } else if (strcmp(dm->get_name()->c_str(), "if_unknown") == 0) {
          const auto& insns = InstructionIterable(dm->get_code());
          TRACE(CONSTP, 1, "%s\n", SHOW(insns));
          bool has_if = false;
          for (auto& mie : insns) {
            IROpcode op = mie.insn->opcode();
            if (is_conditional_branch(op)) {
              has_if = true;
            }
          }
          EXPECT_TRUE(has_if);
        }
      }
    }
  }
}
