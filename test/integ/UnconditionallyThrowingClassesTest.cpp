/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "ScopedCFG.h"

class UnconditionallyThrowingClassesTest : public RedexIntegrationTest {};

/**
 * Test that a synthetic class with a clinit that directly throws is flagged.
 * Since Java source code cannot easily produce clinits that directly throw,
 * we use IRAssembler to create synthetic bytecode.
 */
TEST_F(UnconditionallyThrowingClassesTest, syntheticUnconditionalThrowFlagged) {
  // Create a synthetic class with a clinit that unconditionally throws
  ClassCreator creator(DexType::make_type("LSyntheticThrowingClass;"));
  creator.set_super(type::java_lang_Object());

  // Create a clinit that directly throws:
  // new-instance RuntimeException
  // invoke-direct RuntimeException.<init>
  // throw
  auto* clinit = assembler::method_from_string(R"(
    (method (public static) "LSyntheticThrowingClass;.<clinit>:()V"
     (
      (new-instance "Ljava/lang/RuntimeException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/RuntimeException;.<init>:()V")
      (throw v0)
     )
    )
  )");
  creator.add_method(clinit);
  creator.create();

  // Verify the clinit is detected as unconditionally throwing
  auto* code = clinit->get_code();
  ASSERT_NE(code, nullptr) << "clinit has no code";

  cfg::ScopedCFG cfg(code);
  EXPECT_TRUE(cfg::block_eventually_throws(cfg->entry_block()))
      << "Synthetic class's clinit should be flagged as unconditionally "
         "throwing";
}

/**
 * Test that normal clinits are not flagged as unconditionally throwing.
 */
TEST_F(UnconditionallyThrowingClassesTest, normalClinitNotFlagged) {
  auto scope = build_class_scope(stores);

  auto* cls = find_class(scope, "Lcom/facebook/redextest/NormalClinitClass;");
  ASSERT_NE(cls, nullptr) << "Could not find NormalClinitClass";

  auto* clinit = cls->get_clinit();
  ASSERT_NE(clinit, nullptr) << "NormalClinitClass has no clinit";

  auto* code = clinit->get_code();
  ASSERT_NE(code, nullptr) << "clinit has no code";

  cfg::ScopedCFG cfg(code);
  EXPECT_FALSE(cfg::block_eventually_throws(cfg->entry_block()))
      << "NormalClinitClass's clinit should NOT unconditionally throw";
}

/**
 * Test that conditional throws in clinit are not flagged.
 */
TEST_F(UnconditionallyThrowingClassesTest, conditionalThrowNotFlagged) {
  auto scope = build_class_scope(stores);

  auto* cls =
      find_class(scope, "Lcom/facebook/redextest/ConditionalThrowClinitClass;");
  ASSERT_NE(cls, nullptr) << "Could not find ConditionalThrowClinitClass";

  auto* clinit = cls->get_clinit();
  ASSERT_NE(clinit, nullptr) << "ConditionalThrowClinitClass has no clinit";

  auto* code = clinit->get_code();
  ASSERT_NE(code, nullptr) << "clinit has no code";

  cfg::ScopedCFG cfg(code);
  EXPECT_FALSE(cfg::block_eventually_throws(cfg->entry_block()))
      << "ConditionalThrowClinitClass's clinit should NOT unconditionally "
         "throw since it has a conditional path";
}

/**
 * Test that clinits that invoke methods which throw are not flagged.
 * The pass specifically checks if the entry block itself throws,
 * not if called methods throw.
 */
TEST_F(UnconditionallyThrowingClassesTest, invokeThrowingMethodNotFlagged) {
  auto scope = build_class_scope(stores);

  auto* cls = find_class(
      scope, "Lcom/facebook/redextest/InvokeThrowingMethodClinitClass;");
  ASSERT_NE(cls, nullptr) << "Could not find InvokeThrowingMethodClinitClass";

  auto* clinit = cls->get_clinit();
  ASSERT_NE(clinit, nullptr) << "InvokeThrowingMethodClinitClass has no clinit";

  auto* code = clinit->get_code();
  ASSERT_NE(code, nullptr) << "clinit has no code";

  cfg::ScopedCFG cfg(code);
  // The clinit invokes a method that throws, but the clinit's entry block
  // doesn't directly end in a throw opcode. It ends in an invoke.
  EXPECT_FALSE(cfg::block_eventually_throws(cfg->entry_block()))
      << "InvokeThrowingMethodClinitClass's clinit should NOT be flagged "
         "because it invokes a method rather than throwing directly";
}

/**
 * Test that method call clinits are not flagged.
 */
TEST_F(UnconditionallyThrowingClassesTest, methodCallClinitNotFlagged) {
  auto scope = build_class_scope(stores);

  auto* cls =
      find_class(scope, "Lcom/facebook/redextest/MethodCallClinitClass;");
  ASSERT_NE(cls, nullptr) << "Could not find MethodCallClinitClass";

  auto* clinit = cls->get_clinit();
  ASSERT_NE(clinit, nullptr) << "MethodCallClinitClass has no clinit";

  auto* code = clinit->get_code();
  ASSERT_NE(code, nullptr) << "clinit has no code";

  cfg::ScopedCFG cfg(code);
  EXPECT_FALSE(cfg::block_eventually_throws(cfg->entry_block()))
      << "MethodCallClinitClass's clinit should NOT unconditionally throw";
}

/**
 * Test that classes with no clinit don't cause issues.
 */
TEST_F(UnconditionallyThrowingClassesTest, noClinitClassHandled) {
  auto scope = build_class_scope(stores);

  auto* cls = find_class(scope, "Lcom/facebook/redextest/NoClinitClass;");
  ASSERT_NE(cls, nullptr) << "Could not find NoClinitClass";

  // This class has no clinit, should be handled gracefully
  auto* clinit = cls->get_clinit();
  EXPECT_EQ(clinit, nullptr) << "NoClinitClass should have no clinit";
}

/**
 * Test non-terminating loop is not flagged as throwing.
 */
TEST_F(UnconditionallyThrowingClassesTest, infiniteLoopNotFlagged) {
  ClassCreator creator(DexType::make_type("LSyntheticLoopClass;"));
  creator.set_super(type::java_lang_Object());

  // Create a clinit with infinite loop (doesn't throw)
  auto* clinit = assembler::method_from_string(R"(
    (method (public static) "LSyntheticLoopClass;.<clinit>:()V"
     (
      (:loop)
      (goto :loop)
     )
    )
  )");
  creator.add_method(clinit);
  creator.create();

  auto* code = clinit->get_code();
  ASSERT_NE(code, nullptr) << "clinit has no code";

  cfg::ScopedCFG cfg(code);
  EXPECT_FALSE(cfg::block_eventually_throws(cfg->entry_block()))
      << "Infinite loop should NOT be flagged as unconditionally throwing";
}
