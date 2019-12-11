/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "IRCode.h"
#include "Trace.h"
#include "VerifyUtil.h"
#include "Walkers.h"

TEST_F(PreVerify, InstrumentBBVerify) {
  ASSERT_NE(
      find_class_named(classes,
                       "Lcom/facebook/redextest/InstrumentBasicBlockAnalysis;"),
      nullptr);
  auto cls = find_class_named(
      classes, "Lcom/facebook/redextest/InstrumentBasicBlockTarget;");
  ASSERT_NE(cls, nullptr);

  walk::methods(std::vector<DexClass*>{cls}, [](DexMethod* method) {
    EXPECT_EQ(nullptr,
              find_invoke(method, DOPCODE_INVOKE_STATIC, "onBasicBlockBegin"));
  });
}

TEST_F(PostVerify, InstrumentBBVerify) {
  ASSERT_NE(
      find_class_named(classes,
                       "Lcom/facebook/redextest/InstrumentBasicBlockAnalysis;"),
      nullptr);
  auto cls = find_class_named(
      classes, "Lcom/facebook/redextest/InstrumentBasicBlockTarget;");
  ASSERT_NE(cls, nullptr);

  bool found_testFunc2 = false;
  walk::methods(std::vector<DexClass*>{cls}, [&](DexMethod* method) {
    if (method->get_name()->str() != "testFunc2") {
      return;
    }

    // Check the number of occurences on onBasicBlockBegin() in this method.
    found_testFunc2 = true;
    auto insns = method->get_dex_code()->get_instructions();
    auto insn_iter = insns.begin();
    size_t count_invoke = 0;
    DexOpcodeMethod* prev_invoke = nullptr;
    while (insn_iter++ != insns.end()) {
      DexOpcodeMethod* invoke_instr = find_invoke(
          insn_iter, insns.end(), DOPCODE_INVOKE_STATIC, "onBasicBlockBegin");
      if (invoke_instr && prev_invoke != invoke_instr) {
        prev_invoke = invoke_instr;
        count_invoke += 1;
        // TODO: Verify that invoke_inst is at the beginning of block only.
      }
    }

    // Verifies: Method "testFunc2" has 1 call to onBasicBlockBegin(). There
    // are 3 basic blocks in testFunc2, but only 1 qualifying one.
    // TODO: Verify if number of calls to onBasicBlockBegin() is same as
    // number of qualifying basic blocks.
    EXPECT_EQ(0, count_invoke);
  });

  EXPECT_EQ(true, found_testFunc2);
}
