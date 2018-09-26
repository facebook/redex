/**
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
  verify_class_kept(classes, "Lcom/redex/reachable/A;");
  verify_class_kept(classes, "Lcom/redex/reachable/B;");
  verify_class_kept(classes, "Lcom/redex/reachable/C;");
  verify_class_kept(classes, "Lcom/redex/reachable/D;");
  verify_class_kept(classes, "Lcom/redex/reachable/E;");
  // One known deletion.
  EXPECT_EQ(find_class_named(classes, "Lcom/redex/reachable/DD;"), nullptr);
}
