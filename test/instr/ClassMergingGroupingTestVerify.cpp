/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Show.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, MergeablesRemoval) {
  auto s1 = find_class_named(classes, "Lcom/facebook/redextest/S1;");
  auto s2 = find_class_named(classes, "Lcom/facebook/redextest/S2;");
  auto s3 = find_class_named(classes, "Lcom/facebook/redextest/S3;");
  auto s4 = find_class_named(classes, "Lcom/facebook/redextest/S4;");
  auto s5 = find_class_named(classes, "Lcom/facebook/redextest/S5;");
  auto s6 = find_class_named(classes, "Lcom/facebook/redextest/S6;");
  verify_type_erased(s1);
  verify_type_erased(s2);
  verify_type_erased(s3);
  verify_type_erased(s4);
  verify_type_erased(s5);
  // s6 is not erased because the max_count is 5.
  EXPECT_EQ(s6->get_vmethods().size(), 1);
  EXPECT_EQ(s6->get_dmethods().size(), 1);

  auto q1 = find_class_named(classes, "Lcom/facebook/redextest/Q1;");
  auto q2 = find_class_named(classes, "Lcom/facebook/redextest/Q2;");
  auto q3 = find_class_named(classes, "Lcom/facebook/redextest/Q3;");
  auto q4 = find_class_named(classes, "Lcom/facebook/redextest/Q4;");
  auto q5 = find_class_named(classes, "Lcom/facebook/redextest/Q5;");
  auto q6 = find_class_named(classes, "Lcom/facebook/redextest/Q6;");
  auto q7 = find_class_named(classes, "Lcom/facebook/redextest/Q7;");
  verify_type_erased(q1);
  verify_type_erased(q2);
  verify_type_erased(q3);
  verify_type_erased(q4);
  verify_type_erased(q5);
  verify_type_erased(q6);
  verify_type_erased(q7);
}

TEST_F(PostVerify, ShapeWithGrouping) {
  // The 1st hierarhcy only produce one shape. The trailing subgroup of size 1
  // is not merged.
  auto gb0 = find_class_named(
      classes, "Lcom/facebook/redextest/GroupingBaseShape0S0000000;");
  ASSERT_NE(gb0, nullptr);
  auto gb1 = find_class_named(
      classes, "Lcom/facebook/redextest/GroupingBaseShape1S0000000_1;");
  ASSERT_EQ(gb1, nullptr);

  // The 2nd hierarchy produces two shapes. The size of the trailing subgroup is
  // greater than one.
  auto gs0 = find_class_named(
      classes, "Lcom/facebook/redextest/GroupingSBaseShape1S0000000;");
  ASSERT_NE(gs0, nullptr);
  auto gs1 = find_class_named(
      classes, "Lcom/facebook/redextest/GroupingSBaseShape2S0000000_1;");
  ASSERT_NE(gs1, nullptr);
}
