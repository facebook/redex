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
#include "LocalDce.h"

/*

This test takes as input the Dex bytecode for the class generated
from the Java source file:
   <redex root>/test/integ/ConstantPropagation.java
which is specified in Buck tests via an environment variable in the
BUCK file.

Before optimization, if_false, if_true and if_unknown contain conditional
branches. After the optimization, only if_unknown keeps its condiitonal branch.

This test mainly checks whether the constant propagation is fired.
*/

// The ClassType enum is used to classify and filter classes in test result
enum ClassType {
  MAINCLASS = 0,
  OTHERCLASS = 2,
};

ClassType filter_test_classes(const DexString* cls_name) {
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Propagation;") == 0)
    return MAINCLASS;
  return OTHERCLASS;
}

bool has_conditional_branch(DexMethod* method) {
  const auto& insns = InstructionIterable(method->get_code());
  TRACE(CONSTP, 1, "%s", SHOW(insns));
  for (auto& mie : insns) {
    IROpcode op = mie.insn->opcode();
    if (opcode::is_a_conditional_branch(op)) {
      return true;
    }
  }
  return false;
}

class ConstantPropagationTest : public RedexIntegrationTest {};

TEST_F(ConstantPropagationTest, constantPropagation) {
  std::cout << "Loaded classes: " << classes->size() << std::endl;

  size_t before_methods = 0;
  TRACE(CONSTP, 1, "Code before:");
  for (const auto& cls : *classes) {
    TRACE(CONSTP, 1, "Class %s", SHOW(cls));
    if (filter_test_classes(cls->get_name()) == MAINCLASS) {
      TRACE(CONSTP, 1, "Class %s", SHOW(cls));
      for (const auto& dm : cls->get_dmethods()) {
        TRACE(CONSTP, 1, "dmethod: %s", dm->get_name()->c_str());
        if (strcmp(dm->get_name()->c_str(), "if_false") == 0 ||
            strcmp(dm->get_name()->c_str(), "if_true") == 0 ||
            strcmp(dm->get_name()->c_str(), "if_unknown") == 0) {
          TRACE(CONSTP, 1, "%s", SHOW(InstructionIterable(dm->get_code())));
          before_methods++;
        }
      }
    }
  }
  EXPECT_EQ(before_methods, 3);

  Pass* constp = new ConstantPropagationPass();
  std::vector<Pass*> passes = {constp};

  run_passes(passes);

  size_t after_methods = 0;
  TRACE(CONSTP, 1, "Code after:");
  for (const auto& cls : *classes) {
    TRACE(CONSTP, 1, "Class %s", SHOW(cls));
    if (filter_test_classes(cls->get_name()) == MAINCLASS) {
      for (const auto& dm : cls->get_dmethods()) {
        TRACE(CONSTP, 1, "dmethod: %s", dm->get_name()->c_str());
        if (strcmp(dm->get_name()->c_str(), "if_false") == 0) {
          EXPECT_FALSE(has_conditional_branch(dm));
          after_methods++;
        } else if (strcmp(dm->get_name()->c_str(), "if_true") == 0) {
          EXPECT_FALSE(has_conditional_branch(dm));
          after_methods++;
        } else if (strcmp(dm->get_name()->c_str(), "if_unknown") == 0) {
          EXPECT_TRUE(has_conditional_branch(dm));
          after_methods++;
        }
      }
    }
  }
  EXPECT_EQ(after_methods, 3);
}
