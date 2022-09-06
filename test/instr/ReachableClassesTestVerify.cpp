/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "IRCode.h"
#include "VerifyUtil.h"
#include "Walkers.h"

void verify_class_kept(const DexClasses& classes, const char* name) {
  auto cls = find_class_named(classes, name);
  EXPECT_NE(cls, nullptr) << "Did not find class: " << name;
  EXPECT_NE(find_dmethod_named(*cls, "<init>"), nullptr)
      << "Did not find <init>!";
  EXPECT_NE(find_vmethod_named(*cls, "quack"), nullptr)
      << "Did not find quack!";
}

TEST_F(PostVerify, TestClassesUsedByReflectionKept) {
  // Verify the constructor use cases.
  verify_class_kept(classes, "Lcom/redex/reachable/A;");
  verify_class_kept(classes, "Lcom/redex/reachable/B;");
  verify_class_kept(classes, "Lcom/redex/reachable/C;");
  verify_class_kept(classes, "Lcom/redex/reachable/D;");
  verify_class_kept(classes, "Lcom/redex/reachable/E;");

  // One known class deletion.
  EXPECT_EQ(find_class_named(classes, "Lcom/redex/reachable/DD;"), nullptr);

  // Check distinction between getMethod() and getDeclaredMethod().
  {
    auto cls = find_class_named(classes, "Lcom/redex/reachable/Super;");
    EXPECT_NE(cls, nullptr) << "Did not find class Super!";
    // Should keep a public virtual foo.
    auto vmethods = cls->get_vmethods();
    EXPECT_EQ(vmethods.size(), 1) << "Expected 1 vmethod for Super!";
    // Should have deleted the private dmethod bar.
    auto dmethods = cls->get_dmethods();
    EXPECT_EQ(dmethods.size(), 1) << "Super should only have an <init> method!";
    EXPECT_STREQ(dmethods.front()->c_str(), "<init>")
        << "Super should only have an <init> method!";
  }

  {
    auto cls = find_class_named(classes, "Lcom/redex/reachable/Sub;");
    EXPECT_NE(cls, nullptr) << "Did not find class Sub!";
    // Should keep the public virtuals foo and bar.
    auto vmethods = cls->get_vmethods();
    EXPECT_EQ(vmethods.size(), 2) << "Expected 2 vmethods for Sub!";
    // Should have deleted the private dmethod foo.
    auto dmethods = cls->get_dmethods();
    EXPECT_EQ(dmethods.size(), 2) << "Sub should have 2 dmethods!";
    const auto name0 = dmethods[0]->str();
    const auto name1 = dmethods[1]->str();
    EXPECT_EQ(name0 == "<init>" || name0 == "bar", true)
        << "Unexpected dmethod on class Sub! Got: " << name0;
    EXPECT_EQ(name1 == "<init>" || name1 == "bar", true)
        << "Unexpected dmethod on class Sub! Got: " << name1;
  }
}
