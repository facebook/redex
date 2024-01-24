/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdlib>
#include <gtest/gtest.h>
#include <json/value.h>
#include <memory>
#include <string>

#include "VerifyUtil.h"

TEST_F(PostVerify, EmptyClasses) {
  auto empty_classes =
      find_class_named(classes, "Lcom/facebook/redextest/EmptyClasses;");
  auto inner_empty =
      find_class_named(classes, "Lcom/facebook/redextest/InnerEmpty;");
  auto inner_inner_class = find_class_named(
      classes, "Lcom/facebook/redextest/InnerEmpty$InnerClass;");
  auto inner_empty_2 =
      find_class_named(classes, "Lcom/facebook/redextest/InnerEmpty2;");
  auto inner_inner_class_2 = find_class_named(
      classes, "Lcom/facebook/redextest/InnerEmpty2$InnerClass2;");
  auto not_empty =
      find_class_named(classes, "Lcom/facebook/redextest/NotAnEmptyClass;");
  auto not_empty_2 =
      find_class_named(classes, "Lcom/facebook/redextest/NotAnEmptyClass2;");
  auto not_empty_3 =
      find_class_named(classes, "Lcom/facebook/redextest/NotAnEmptyClass3;");
  auto not_empty_4 =
      find_class_named(classes, "Lcom/facebook/redextest/NotAnEmptyClass4;");
  auto not_empty_5 =
      find_class_named(classes, "Lcom/facebook/redextest/NotAnEmptyClass5;");
  auto yes_no = find_class_named(classes, "Lcom/facebook/redextest/YesNo;");
  auto my_yes_no =
      find_class_named(classes, "Lcom/facebook/redextest/MyYesNo;");
  auto easily_done =
      find_class_named(classes, "Lcom/facebook/redextest/EasilyDone;");
  auto by_20r_3 = find_class_named(classes, "Lcom/facebook/redextest/By2Or3;");
  auto my_by_20r_3 =
      find_class_named(classes, "Lcom/facebook/redextest/MyBy2Or3;");
  auto wombat_exception =
      find_class_named(classes, "Lcom/facebook/redextest/WombatException;");
  auto wombat = find_class_named(classes, "Lcom/facebook/redextest/Wombat;");
  auto empty_but_extended = find_class_named(
      classes, "Lcom/facebook/redextest/EmptyButLaterExtended;");
  auto extender =
      find_class_named(classes, "Lcom/facebook/redextest/Extender;");
  auto not_used_here =
      find_class_named(classes, "Lcom/facebook/redextest/NotUsedHere;");
  auto dont_kill_me_now =
      find_class_named(classes, "Lcom/facebook/redextest/DontKillMeNow;");
  auto numbat_exception =
      find_class_named(classes, "Lcom/facebook/redextest/NumbatException;");

  // The "empty class" isn't actually empty, as it has an <init>.
  EXPECT_NE(nullptr, empty_classes);
  EXPECT_TRUE(empty_classes->has_ctors());

  // Super class will not be removed.
  EXPECT_NE(nullptr, inner_empty);
  EXPECT_NE(nullptr, inner_empty_2);
  EXPECT_NE(nullptr, empty_but_extended);

  // TODO: inner empty classes should have been removed?
  EXPECT_NE(nullptr, inner_inner_class);
  EXPECT_NE(nullptr, inner_inner_class_2);

  // Non empty, used classes should be kept.
  EXPECT_NE(nullptr, not_empty);
  EXPECT_NE(nullptr, not_empty_2);
  EXPECT_NE(nullptr, not_empty_3);
  EXPECT_NE(nullptr, not_empty_4);
  EXPECT_NE(nullptr, not_empty_5);

  // Interfaces should be kept.
  EXPECT_NE(nullptr, yes_no);
  EXPECT_NE(nullptr, easily_done);
  EXPECT_NE(nullptr, not_used_here);

  // Used in the main program should be kept.
  EXPECT_NE(nullptr, my_yes_no);
  EXPECT_NE(nullptr, by_20r_3);
  EXPECT_NE(nullptr, my_by_20r_3);
  EXPECT_NE(nullptr, wombat);
  EXPECT_NE(nullptr, extender);

  // Exceptions thrown from kept methods, should be kept.
  EXPECT_NE(nullptr, wombat_exception);

  // TODO: exceptions thrown from removed classes, should be removed.
  // EXPECT_EQ(nullptr, numbat_exception);

  // Classes that have annotations that should be kept, should be kept.
  EXPECT_NE(nullptr, dont_kill_me_now);
}
