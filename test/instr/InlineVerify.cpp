/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "VerifyUtil.h"

/*
 * Ensure that testInvokeRange() is actually testing inlined code by checking
 * that the invoke-direct/range opcode is removed in the redexed binary.
 */

TEST_F(PreVerify, InlineInvokeRange) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  ASSERT_NE(nullptr, cls);

  auto m = find_vmethod_named(*cls, "testInvokeRange");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, OPCODE_INVOKE_DIRECT_RANGE,
        "needsInvokeRange"));
}

TEST_F(PostVerify, InlineInvokeRange) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  ASSERT_NE(nullptr, cls);

  auto m = find_vmethod_named(*cls, "testInvokeRange");
  ASSERT_NE(nullptr, m);
  ASSERT_EQ(nullptr, find_invoke(m, OPCODE_INVOKE_DIRECT_RANGE,
        "needsInvokeRange"));
}

/*
 * Ensure that testCallerTryCalleeElseThrows() is testing inlined code.
 */

TEST_F(PreVerify, InlineCallerTryCalleeElseThrows) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeElseThrows");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInElse");
  ASSERT_NE(nullptr, invoke);

  // verify that the callee has an if-else statement, and that the else block
  // (which throws an exception) comes after the return opcode... meaning that
  // for the instrumentation test to pass, we must duplicate the caller try
  // item
  auto callee_insns = invoke->get_method()->get_code()->get_instructions();
  auto retop = std::find_if(callee_insns.begin(), callee_insns.end(),
    [](DexInstruction* insn) {
      return insn->opcode() == OPCODE_RETURN_VOID;
    });
  ASSERT_NE(callee_insns.end(), retop);
  auto invoke_throw =
    find_invoke(retop, callee_insns.end(), OPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throw);

  auto code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCallerTryCalleeElseThrows) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeElseThrows");
  // verify that we've removed the throwsInElse() call
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInElse");
  ASSERT_EQ(nullptr, invoke);

  auto code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 2);
  // verify that we haven't increased the number of catch handlers -- both
  // try blocks should point to the same handler
}

/*
 * Ensure that testCallerTryCalleeIfThrows() is testing inlined code.
 * I don't expect this case to be too tricky -- unlike the ElseThrows case,
 * we don't need to duplicate any try items for the instr test to pass.
 * Nevertheless, I'm including it here for completeness.
 */

TEST_F(PreVerify, InlineCallerTryCalleeIfThrows) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeIfThrows");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInIf");
  ASSERT_NE(nullptr, invoke);

  // verify that the callee has an if-else statement, and that the if block
  // (which throws an exception) comes before the return opcode
  auto callee_insns = invoke->get_method()->get_code()->get_instructions();
  auto ifop = std::find_if(callee_insns.begin(), callee_insns.end(),
    [](DexInstruction* insn) {
      return insn->opcode() == OPCODE_IF_NEZ;
    });
  ASSERT_NE(callee_insns.end(), ifop);
  auto retop = std::find_if(callee_insns.begin(), callee_insns.end(),
    [](DexInstruction* insn) {
      return insn->opcode() == OPCODE_RETURN_VOID;
    });
  ASSERT_NE(callee_insns.end(), retop);
  auto invoke_throw =
    find_invoke(ifop, retop, OPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throw);

  auto code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCallerTryCalleeIfThrows) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeElseThrows");
  // verify that we've removed the throwsInIf() call
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInIf");
  ASSERT_EQ(nullptr, invoke);

  auto code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 2);
}

/*
 * Ensure that testCallerTryCalleeNestedTry() is testing inlined code.
 * I don't expect this case to be particularly tricky; just including for
 * completeness.
 */

TEST_F(PreVerify, InlineCallerNestedTry) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerNestedTry");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInElse2");
  ASSERT_NE(nullptr, invoke);

  auto code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 2);
}

TEST_F(PostVerify, InlineCallerNestedTry) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerNestedTry");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInElse2");
  ASSERT_EQ(nullptr, invoke);

  auto code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 3);
}
