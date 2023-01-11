/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Resolver.h"
#include "Show.h"
#include "verify/VerifyUtil.h"

namespace {
constexpr const char* class_A = "Lcom/facebook/redextest/A;";
constexpr const char* class_B = "Lcom/facebook/redextest/B;";
constexpr const char* class_C = "Lcom/facebook/redextest/C;";
constexpr const char* class_D = "Lcom/facebook/redextest/D;";
constexpr const char* class_E = "Lcom/facebook/redextest/E;";
} // namespace

TEST_F(PreVerify, SortRemainingClass) {
  // Before opt, the class order is A->B->C->D->E.
  EXPECT_TRUE(find_class_idx(classes, class_A) <
              find_class_idx(classes, class_B));
  EXPECT_TRUE(find_class_idx(classes, class_B) <
              find_class_idx(classes, class_C));
  EXPECT_TRUE(find_class_idx(classes, class_C) <
              find_class_idx(classes, class_D));
  EXPECT_TRUE(find_class_idx(classes, class_D) <
              find_class_idx(classes, class_E));
}

TEST_F(PostVerify, SortRemainingClass) {
  // After opt, the class order is E->B->A->C->D
  // class_B has 11 vmethod, which class_E has 6.
  EXPECT_TRUE(find_class_idx(classes, class_E) <
              find_class_idx(classes, class_B));

  // class_B has 1 dmethod, while class_A has 2.
  EXPECT_TRUE(find_class_idx(classes, class_B) <
              find_class_idx(classes, class_A));

  // class_A and class_C have different interfaces, sort by interfaces.
  EXPECT_TRUE(find_class_idx(classes, class_A) <
              find_class_idx(classes, class_C));

  // class_D has 2 dmethods, while class_C has 1.
  EXPECT_TRUE(find_class_idx(classes, class_C) <
              find_class_idx(classes, class_D));
}
