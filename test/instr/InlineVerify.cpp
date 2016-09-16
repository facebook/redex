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
#include "Show.h"
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

  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCallerTryCalleeElseThrows) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeElseThrows");
  // verify that we've removed the throwsInElse() call
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInElse");
  ASSERT_EQ(nullptr, invoke);

  auto& code = m->get_code();
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

  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCallerTryCalleeIfThrows) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeElseThrows");
  // verify that we've removed the throwsInIf() call
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInIf");
  ASSERT_EQ(nullptr, invoke);

  auto& code = m->get_code();
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

  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 2);
}

TEST_F(PostVerify, InlineCallerNestedTry) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerNestedTry");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsInElse2");
  ASSERT_EQ(nullptr, invoke);

  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 3);
}

/*
 * Ensure that testCalleeTryUncaught() is testing inlined code.
 */

TEST_F(PreVerify, InlineCalleeTryUncaught) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryUncaught");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsUncaught");
  ASSERT_NE(nullptr, invoke);
  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCalleeTryUncaught) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryUncaught");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsUncaught");
  ASSERT_EQ(nullptr, invoke);
  auto invoke_throws = find_invoke(m, OPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throws);
  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 2);
}

/*
 * Ensure that testCalleeTryCaught() is testing inlined code.
 */

TEST_F(PreVerify, InlineCalleeTryCaught) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryCaught");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsCaught");
  ASSERT_NE(nullptr, invoke);
  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCalleeTryCaught) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryCaught");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "throwsCaught");
  ASSERT_EQ(nullptr, invoke);
  auto invoke_throws = find_invoke(m, OPCODE_INVOKE_VIRTUAL,
      "wrapsArithmeticThrow");
  ASSERT_NE(nullptr, invoke_throws);
  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 2);
}

/*
 * Ensure that testCalleeHandlerThrows() is testing inlined code.
 */

TEST_F(PreVerify, InlineTryHandlerThrows) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryHandlerThrows");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "handlerThrows");
  ASSERT_NE(nullptr, invoke);
  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineTryHandlerThrows) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryHandlerThrows");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "handlerThrows");
  ASSERT_EQ(nullptr, invoke);
  auto invoke_throws = find_invoke(m, OPCODE_INVOKE_VIRTUAL,
      "wrapsArithmeticThrow");
  ASSERT_NE(nullptr, invoke_throws);
  invoke_throws = find_invoke(m, OPCODE_INVOKE_VIRTUAL,
      "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throws);
  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 2);
}

/*
 * Ensure that testInlineCalleeTryTwice() is testing inlined code.
 */

TEST_F(PreVerify, InlineCalleeTryTwice) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineCalleeTryTwice");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "inlineCalleeTryTwice");
  ASSERT_NE(nullptr, invoke);
  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCalleeTryTwice) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineCalleeTryTwice");
  auto invoke = find_invoke(m, OPCODE_INVOKE_DIRECT, "inlineCalleeTryTwice");
  ASSERT_EQ(nullptr, invoke);
  auto invoke_throws = find_invoke(m, OPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throws);
  auto& code = m->get_code();
  ASSERT_EQ(code->get_tries().size(), 3);
}

/*
 * Ensure that testInlineInvokeDirect() is testing inlined code.
 */

TEST_F(PreVerify, InlineInvokeDirect) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineInvokeDirect");
  auto invoke =
      find_invoke(m, OPCODE_INVOKE_DIRECT, "hasNoninlinableInvokeDirect");
  auto noninlinable_invoke_direct =
      find_invoke(invoke->get_method(), OPCODE_INVOKE_DIRECT, "noninlinable");
  auto noninlinable = noninlinable_invoke_direct->get_method();
  ASSERT_EQ(show(noninlinable->get_proto()), "()V");

  // verify that there are two inlinable() methods in the class. The static
  // version exists to test that we don't cause a signature collision when we
  // make the instance method static.
  auto dmethods = cls->get_dmethods();
  ASSERT_EQ(2,
            std::count_if(dmethods.begin(), dmethods.end(), [](DexMethod* m) {
              return !strcmp("noninlinable", m->get_name()->c_str());
            }));
}

TEST_F(PostVerify, InlineInvokeDirect) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineInvokeDirect");
  auto noninlinable_invoke_direct =
      find_invoke(m, OPCODE_INVOKE_STATIC, "noninlinable$redex0");
  ASSERT_NE(nullptr, noninlinable_invoke_direct);
  auto noninlinable = noninlinable_invoke_direct->get_method();
  ASSERT_EQ(show(noninlinable->get_proto()),
            "(Lcom/facebook/redexinline/InlineTest;)V");

  auto dmethods = cls->get_dmethods();
  // verify that we've replaced the instance noninlinable() method with
  // noninlinable$redex
  ASSERT_EQ(1,
            std::count_if(dmethods.begin(), dmethods.end(), [](DexMethod* m) {
              return !strcmp("noninlinable", m->get_name()->c_str());
            }));
  ASSERT_EQ(1,
            std::count_if(dmethods.begin(), dmethods.end(), [](DexMethod* m) {
              return !strcmp("noninlinable$redex0", m->get_name()->c_str());
            }));
}

/*
 * Ensure that pseudo-opcodes remain at the end of the caller.
 */

TEST_F(PreVerify, testArrayDataInCaller) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testArrayDataInCaller");

  // check that the callee indeed has a non-terminal if, which will exercise
  // the inliner code path that searches for fopcodes in the caller
  auto callee = find_invoke(m, OPCODE_INVOKE_DIRECT, "calleeWithIf");
  auto insns = callee->get_method()->get_code()->get_instructions();
  auto ret_it =
      std::find_if(insns.begin(), insns.end(), [&](DexInstruction* insn) {
        return is_return(insn->opcode());
      });
  ASSERT_NE(ret_it, insns.end());

  auto last_insn = m->get_code()->get_instructions().back();
  ASSERT_EQ(last_insn->opcode(), FOPCODE_FILLED_ARRAY);
}

TEST_F(PostVerify, testArrayDataInCaller) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testArrayDataInCaller");
  ASSERT_EQ(nullptr, find_invoke(m, OPCODE_INVOKE_DIRECT, "callerWithIf"));
  auto last_insn = m->get_code()->get_instructions().back();
  ASSERT_EQ(last_insn->opcode(), FOPCODE_FILLED_ARRAY);
}

TEST_F(PostVerify, testForceInline) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  auto m = find_vmethod_named(*cls, "testForceInlineOne");
  EXPECT_EQ(nullptr, find_invoke(m, OPCODE_INVOKE_DIRECT, "multipleCallers"));
  m = find_vmethod_named(*cls, "testForceInlineTwo");
  EXPECT_EQ(nullptr, find_invoke(m, OPCODE_INVOKE_DIRECT, "multipleCallers"));
}
