/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "Trace.h"

#include "ConstantPropagationPass.h"
#include "DelInit.h"
#include "LocalDce.h"
#include "RemoveEmptyClasses.h"

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

ClassType filter_test_classes(const DexString* cls_name) {
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Propagation;") == 0)
    return MAINCLASS;
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Alpha;") == 0)
    return REMOVEDCLASS;
  return OTHERCLASS;
}

class ConstantPropagationTest : public RedexIntegrationTest {};

TEST_F(ConstantPropagationTest, constantPropagation) {
  std::cout << "Loaded classes: " << classes->size() << std::endl;

  TRACE(CONSTP, 1, "Code before:");
  for (const auto& cls : *classes) {
    TRACE(CONSTP, 1, "Class %s", SHOW(cls));
    if (filter_test_classes(cls->get_name()) < 2) {
      TRACE(CONSTP, 1, "Class %s", SHOW(cls));
      for (const auto& dm : cls->get_dmethods()) {
        TRACE(CONSTP, 1, "dmethod: %s", dm->get_name()->c_str());
        if (strcmp(dm->get_name()->c_str(), "if_false") == 0 ||
            strcmp(dm->get_name()->c_str(), "if_true") == 0 ||
            strcmp(dm->get_name()->c_str(), "if_unknown") == 0) {
          TRACE(CONSTP, 1, "%s", SHOW(InstructionIterable(dm->get_code())));
        }
      }
    }
  }

  Pass* constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {constp};

  run_passes(passes);

  TRACE(CONSTP, 1, "Code after:");
  for (const auto& cls : *classes) {
    TRACE(CONSTP, 1, "Class %s", SHOW(cls));
    if (filter_test_classes(cls->get_name()) == MAINCLASS) {
      for (const auto& dm : cls->get_dmethods()) {
        TRACE(CONSTP, 1, "dmethod: %s", dm->get_name()->c_str());
        if (strcmp(dm->get_name()->c_str(), "if_false") == 0) {
          const auto& insns = InstructionIterable(dm->get_code());
          TRACE(CONSTP, 1, "%s", SHOW(insns));
          for (auto& mie : insns) {
            IROpcode op = mie.insn->opcode();
            EXPECT_NE(op, OPCODE_IF_EQZ);
          }
        } else if (strcmp(dm->get_name()->c_str(), "if_true") == 0) {
          const auto& insns = InstructionIterable(dm->get_code());
          TRACE(CONSTP, 1, "%s", SHOW(insns));
          for (auto& mie : insns) {
            IROpcode op = mie.insn->opcode();
            EXPECT_NE(op, OPCODE_IF_EQZ);
          }
        } else if (strcmp(dm->get_name()->c_str(), "if_unknown") == 0) {
          const auto& insns = InstructionIterable(dm->get_code());
          TRACE(CONSTP, 1, "%s", SHOW(insns));
          bool has_if = false;
          for (auto& mie : insns) {
            IROpcode op = mie.insn->opcode();
            if (opcode::is_a_conditional_branch(op)) {
              has_if = true;
            }
          }
          EXPECT_TRUE(has_if);
        }
      }
    }
  }
}
