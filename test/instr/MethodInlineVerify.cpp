/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  ASSERT_NE(nullptr, cls);

  auto m = find_vmethod_named(*cls, "testInvokeRange");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr,
            find_invoke(m, DOPCODE_INVOKE_DIRECT_RANGE, "needsInvokeRange"));
}

TEST_F(PostVerify, InlineInvokeRange) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  ASSERT_NE(nullptr, cls);

  auto m = find_vmethod_named(*cls, "testInvokeRange");
  ASSERT_NE(nullptr, m);
  ASSERT_EQ(nullptr,
            find_invoke(m, DOPCODE_INVOKE_DIRECT_RANGE, "needsInvokeRange"))
      << show(m->get_dex_code());
}

/*
 * Ensure that testCallerTryCalleeElseThrows() is testing inlined code.
 */

TEST_F(PreVerify, InlineCallerTryCalleeElseThrows) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeElseThrows");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsInElse");
  ASSERT_NE(nullptr, invoke);

  // verify that the callee has an if-else statement, and that the else block
  // (which throws an exception) comes after the return opcode... meaning that
  // for the instrumentation test to pass, we must duplicate the caller try
  // item
  ASSERT_TRUE(invoke->get_method()->is_def());
  auto callee_insns =
      invoke->get_method()->as_def()->get_dex_code()->get_instructions();
  auto retop = std::find_if(callee_insns.begin(), callee_insns.end(),
                            [](DexInstruction* insn) {
                              return insn->opcode() == DOPCODE_RETURN_VOID;
                            });
  ASSERT_NE(callee_insns.end(), retop);
  auto invoke_throw = find_invoke(retop, callee_insns.end(),
                                  DOPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throw);

  auto code = m->get_dex_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCallerTryCalleeElseThrows) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeElseThrows");
  // verify that we've removed the throwsInElse() call
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsInElse");
  ASSERT_EQ(nullptr, invoke);

  auto code = m->get_dex_code();
  ASSERT_LE(code->get_tries().size(), 2) << show(code);
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
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeIfThrows");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsInIf");
  ASSERT_NE(nullptr, invoke);

  // verify that the callee has an if-else statement, and that the if block
  // (which throws an exception) comes before the return opcode
  ASSERT_TRUE(invoke->get_method()->is_def());
  auto callee_insns =
      invoke->get_method()->as_def()->get_dex_code()->get_instructions();
  auto ifop = std::find_if(
      callee_insns.begin(), callee_insns.end(),
      [](DexInstruction* insn) { return insn->opcode() == DOPCODE_IF_NEZ; });
  ASSERT_NE(callee_insns.end(), ifop);
  auto retop = std::find_if(callee_insns.begin(), callee_insns.end(),
                            [](DexInstruction* insn) {
                              return insn->opcode() == DOPCODE_RETURN_VOID;
                            });
  ASSERT_NE(callee_insns.end(), retop);
  auto invoke_throw =
      find_invoke(ifop, retop, DOPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throw);

  auto code = m->get_dex_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCallerTryCalleeIfThrows) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerTryCalleeElseThrows");
  // verify that we've removed the throwsInIf() call
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsInIf");
  ASSERT_EQ(nullptr, invoke);

  auto code = m->get_dex_code();
  ASSERT_LE(code->get_tries().size(), 2);
}

/*
 * Ensure that testCallerTryCalleeNestedTry() is testing inlined code.
 * I don't expect this case to be particularly tricky; just including for
 * completeness.
 */

TEST_F(PreVerify, InlineCallerNestedTry) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerNestedTry");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsInElse2");
  ASSERT_NE(nullptr, invoke);

  auto code = m->get_dex_code();
  ASSERT_LE(code->get_tries().size(), 2);
}

TEST_F(PostVerify, InlineCallerNestedTry) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCallerNestedTry");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsInElse2");
  ASSERT_EQ(nullptr, invoke);

  auto code = m->get_dex_code();
  ASSERT_LE(code->get_tries().size(), 3);
}

/*
 * Ensure that testCalleeTryUncaught() is testing inlined code.
 */

TEST_F(PreVerify, InlineCalleeTryUncaught) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryUncaught");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsUncaught");
  ASSERT_NE(nullptr, invoke);
  auto code = m->get_dex_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCalleeTryUncaught) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryUncaught");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsUncaught");
  ASSERT_EQ(nullptr, invoke);
  auto invoke_throws = find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throws);
  auto code = m->get_dex_code();
  ASSERT_LE(code->get_tries().size(), 2);
}

/*
 * Ensure that testCalleeTryCaught() is testing inlined code.
 */

TEST_F(PreVerify, InlineCalleeTryCaught) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryCaught");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsCaught");
  ASSERT_NE(nullptr, invoke);
  auto code = m->get_dex_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCalleeTryCaught) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryCaught");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "throwsCaught");
  ASSERT_EQ(nullptr, invoke);
  auto invoke_throws =
      find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "wrapsArithmeticThrow");
  ASSERT_NE(nullptr, invoke_throws);
  auto code = m->get_dex_code();
  ASSERT_LE(code->get_tries().size(), 2);
}

/*
 * Ensure that testCalleeHandlerThrows() is testing inlined code.
 */

TEST_F(PreVerify, InlineTryHandlerThrows) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryHandlerThrows");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "handlerThrows");
  ASSERT_NE(nullptr, invoke);
  auto code = m->get_dex_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineTryHandlerThrows) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeTryHandlerThrows");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "handlerThrows");
  ASSERT_EQ(nullptr, invoke);
  auto invoke_throws =
      find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "wrapsArithmeticThrow");
  ASSERT_NE(nullptr, invoke_throws);
  invoke_throws = find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throws);
  auto code = m->get_dex_code();
  ASSERT_EQ(code->get_tries().size(), 2);
}

/*
 * Ensure that testInlineCalleeTryTwice() is testing inlined code.
 */

TEST_F(PreVerify, InlineCalleeTryTwice) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineCalleeTryTwice");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "inlineCalleeTryTwice");
  ASSERT_NE(nullptr, invoke);
  auto code = m->get_dex_code();
  ASSERT_EQ(code->get_tries().size(), 1);
}

TEST_F(PostVerify, InlineCalleeTryTwice) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineCalleeTryTwice");
  auto invoke = find_invoke(m, DOPCODE_INVOKE_DIRECT, "inlineCalleeTryTwice");
  ASSERT_EQ(nullptr, invoke);
  auto invoke_throws = find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "wrapsThrow");
  ASSERT_NE(nullptr, invoke_throws);
  auto code = m->get_dex_code();
  ASSERT_EQ(code->get_tries().size(), 3);
}

/*
 * Ensure that testInlineInvokeDirect() is testing inlined code.
 */

TEST_F(PreVerify, InlineInvokeDirect) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineInvokeDirect");
  auto invoke =
      find_invoke(m, DOPCODE_INVOKE_DIRECT, "hasNoninlinableInvokeDirect");
  ASSERT_TRUE(invoke->get_method()->is_def());
  auto noninlinable_invoke_direct =
      find_invoke(static_cast<DexMethod*>(invoke->get_method()),
                  DOPCODE_INVOKE_DIRECT, "noninlinable");
  ASSERT_TRUE(noninlinable_invoke_direct->get_method()->is_def());
  auto noninlinable =
      static_cast<DexMethod*>(noninlinable_invoke_direct->get_method());
  ASSERT_EQ(show(noninlinable->get_proto()), "()V");

  // verify that there is one inlinable() method in the class.
  auto dmethods = cls->get_dmethods();
  ASSERT_EQ(1,
            std::count_if(dmethods.begin(), dmethods.end(), [](DexMethod* m) {
              return !strcmp("noninlinable", m->get_name()->c_str());
            }));
}

TEST_F(PostVerify, InlineInvokeDirect) {
  // verify that the content of hasNoninlinableInvokeDirect has been inlined,
  // but noninlinable did not get turned into a static method.
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineInvokeDirect");
  auto noninlinable_invoke_direct =
      find_invoke(m, DOPCODE_INVOKE_DIRECT, "noninlinable");
  EXPECT_NE(nullptr, noninlinable_invoke_direct) << show(m->get_dex_code());
  ASSERT_TRUE(noninlinable_invoke_direct->get_method()->is_def());
  auto noninlinable =
      static_cast<DexMethod*>(noninlinable_invoke_direct->get_method());
  EXPECT_EQ(show(noninlinable->get_proto()), "()V");

  // verify that there is (still) one direct "noninlinable" method in the class.
  auto dmethods = cls->get_dmethods();
  ASSERT_EQ(1,
            std::count_if(dmethods.begin(), dmethods.end(), [](DexMethod* m) {
              return !strcmp("noninlinable", m->get_name()->c_str());
            }));
}

/*
 * Ensure that testInlineInvokeDirectAcrossClasses() is testing inlined code.
 */

TEST_F(PreVerify, InlineInvokeDirectCrossClasses) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineInvokeDirectAcrossClasses");
  auto invoke =
      find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "hasNoninlinableInvokeDirect");
  ASSERT_TRUE(invoke->get_method()->is_def());
  auto noninlinable_invoke_direct =
      find_invoke(static_cast<DexMethod*>(invoke->get_method()),
                  DOPCODE_INVOKE_DIRECT, "noninlinable");
  ASSERT_TRUE(noninlinable_invoke_direct->get_method()->is_def());
  auto noninlinable = noninlinable_invoke_direct->get_method()->as_def();
  ASSERT_EQ(show(noninlinable->get_proto()), "()V");

  // verify that there are two inlinable() methods in the class. The static
  // version exists to test that we don't cause a signature collision when we
  // make the instance method static.
  auto nested_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineTest$OtherClass;");
  auto dmethods = nested_cls->get_dmethods();
  ASSERT_EQ(2,
            std::count_if(dmethods.begin(), dmethods.end(), [](DexMethod* m) {
              return !strcmp("noninlinable", m->get_name()->c_str());
            }));
}

TEST_F(PostVerify, InlineInvokeDirectCrossClasses) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testInlineInvokeDirectAcrossClasses");
  auto noninlinable_invoke_direct =
      find_invoke(m, DOPCODE_INVOKE_STATIC, "noninlinable$0");
  EXPECT_NE(nullptr, noninlinable_invoke_direct) << show(m->get_dex_code());
  ASSERT_TRUE(noninlinable_invoke_direct->get_method()->is_def());
  auto noninlinable =
      static_cast<DexMethod*>(noninlinable_invoke_direct->get_method());
  EXPECT_EQ(show(noninlinable->get_proto()),
            "(Lcom/facebook/redexinline/MethodInlineTest$OtherClass;)V");

  auto nested_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineTest$OtherClass;");
  auto dmethods = nested_cls->get_dmethods();
  // verify that we've replaced the instance noninlinable() method with r$0
  ASSERT_EQ(1,
            std::count_if(dmethods.begin(), dmethods.end(), [](DexMethod* m) {
              return m->get_name()->str() == "noninlinable";
            }));
}

/*
 * Ensure that pseudo-opcodes remain at the end of the caller.
 */

TEST_F(PreVerify, testArrayDataInCaller) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testArrayDataInCaller");

  // check that the callee indeed has a non-terminal if, which will exercise
  // the inliner code path that searches for fopcodes in the caller
  auto callee = find_invoke(m, DOPCODE_INVOKE_DIRECT, "calleeWithIf");
  ASSERT_TRUE(callee->get_method()->is_def());
  auto insns =
      callee->get_method()->as_def()->get_dex_code()->get_instructions();
  auto ret_it =
      std::find_if(insns.begin(), insns.end(), [&](DexInstruction* insn) {
        return insn->opcode() == DOPCODE_RETURN_VOID;
      });
  ASSERT_NE(ret_it, insns.end());

  auto last_insn = m->get_dex_code()->get_instructions().back();
  ASSERT_EQ(last_insn->opcode(), FOPCODE_FILLED_ARRAY);
}

TEST_F(PostVerify, testArrayDataInCaller) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testArrayDataInCaller");
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_DIRECT, "callerWithIf"));
  auto last_insn = m->get_dex_code()->get_instructions().back();
  ASSERT_EQ(last_insn->opcode(), FOPCODE_FILLED_ARRAY);
}

TEST_F(PostVerify, testForceInline) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testForceInlineOne");
  EXPECT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_DIRECT, "multipleCallers"));
  m = find_vmethod_named(*cls, "testForceInlineTwo");
  EXPECT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_DIRECT, "multipleCallers"));
}

TEST_F(PreVerify, testCalleeRefsPrivateClass) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeRefsPrivateClass");
  EXPECT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "inlineMe"));

  auto other_pkg_cls = find_class_named(
      classes,
      "Lcom/facebook/redexinline/otherpackage/MethodInlineOtherPackage$Bar;");
  EXPECT_FALSE(is_public(other_pkg_cls));
}

TEST_F(PostVerify, testCalleeRefsPrivateClass) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testCalleeRefsPrivateClass");
  EXPECT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "inlineMe"));
  auto other_pkg_cls = find_class_named(
      classes,
      "Lcom/facebook/redexinline/otherpackage/MethodInlineOtherPackage$Bar;");
  EXPECT_TRUE(is_public(other_pkg_cls));
}

TEST_F(PreVerify, testFillArrayOpcode) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testFillArrayOpcode");
  EXPECT_NE(nullptr,
            find_invoke(m, DOPCODE_INVOKE_DIRECT, "calleeWithFillArray"));
}

TEST_F(PostVerify, testFillArrayOpcode) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_vmethod_named(*cls, "testFillArrayOpcode");
  EXPECT_EQ(nullptr,
            find_invoke(m, DOPCODE_INVOKE_DIRECT, "calleeWithFillArray"));
}

TEST_F(PreVerify, testUpdateCodeSizeWhenInlining) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto m = find_dmethod_named(*cls, "smallMethodThatBecomesBig");
  EXPECT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_DIRECT, "bigMethod"));
}

TEST_F(PostVerify, testUpdateCodeSizeWhenInlining) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  auto small = find_dmethod_named(*cls, "smallMethodThatBecomesBig");
  EXPECT_NE(small, nullptr)
      << "smallMethodThatBecomesBig should not be inlined!";
  EXPECT_EQ(nullptr, find_invoke(small, DOPCODE_INVOKE_DIRECT, "bigMethod"));
}

TEST_F(PreVerify, testFinallyEmpty) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "callEmpty");
  ASSERT_NE(nullptr, m);
  EXPECT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "cleanup"))
      << SHOW(m->get_dex_code());
}

TEST_F(PostVerify, testFinallyEmpty) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "callEmpty");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "cleanup"));
}

TEST_F(PostVerify, inlineAcrossCallerNoApi) {
  // Make sure we're still calling all the api specific methods. Make sure they
  // haven't been inlined.
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "callSpecificApi");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ(nullptr,
            find_invoke(m, DOPCODE_INVOKE_STATIC, "shouldInlineMinSdk"));
  EXPECT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_STATIC, "useApi"));
  EXPECT_NE(nullptr,
            find_invoke(m, DOPCODE_INVOKE_STATIC, "shouldNotInlineOutOfClass"));
  EXPECT_NE(nullptr,
            find_invoke(m, DOPCODE_INVOKE_STATIC, "shouldInlineNintoO"));
  EXPECT_NE(nullptr,
            find_invoke(m, DOPCODE_INVOKE_STATIC, "shouldNotInlineOintoN"));
  EXPECT_EQ(nullptr,
            find_invoke(m, DOPCODE_INVOKE_STATIC, "doesntActuallyNeedN"));
}

TEST_F(PostVerify, inlineAcrossCallerAndroidN) {
  auto n = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineTest$NeedsAndroidN;");
  ASSERT_NE(nullptr, n);

  auto useApi = find_dmethod_named(*n, "useApi");
  ASSERT_NE(nullptr, useApi);

  auto shouldNotInlineOintoN = find_dmethod_named(*n, "shouldNotInlineOintoN");
  ASSERT_NE(nullptr, shouldNotInlineOintoN);
  EXPECT_NE(nullptr, find_invoke(shouldNotInlineOintoN, DOPCODE_INVOKE_STATIC,
                                 "useApiO"));
}

TEST_F(PostVerify, inlineAcrossCallerAndroidO) {
  auto o = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineTest$NeedsAndroidO;");
  ASSERT_NE(nullptr, o);

  auto shouldInlineWithinClass =
      find_dmethod_named(*o, "shouldInlineWithinClass");
  // Should be gone.
  EXPECT_EQ(nullptr, shouldInlineWithinClass);

  auto shouldInlineNintoO = find_dmethod_named(*o, "shouldInlineNintoO");
  ASSERT_NE(nullptr, shouldInlineNintoO);
  // Should be inlined. No callsite.
  EXPECT_EQ(nullptr,
            find_invoke(shouldInlineNintoO, DOPCODE_INVOKE_STATIC, "useApi"));
}
