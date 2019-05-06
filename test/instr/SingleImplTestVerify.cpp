/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "verify/VerifyUtil.h"

TEST_F(PreVerify, test) {
  ASSERT_NE(find_class_named(classes, "Lcom/facebook/redextest/I_1;"), nullptr);
  ASSERT_NE(find_class_named(classes, "Lcom/facebook/redextest/I_2;"), nullptr);
  ASSERT_NE(find_class_named(classes, "Lcom/facebook/redextest/I_3;"), nullptr);
}

TEST_F(PostVerify, test) {
  ASSERT_EQ(find_class_named(classes, "Lcom/facebook/redextest/I_1;"), nullptr);
  ASSERT_EQ(find_class_named(classes, "Lcom/facebook/redextest/I_2;"), nullptr);
  ASSERT_EQ(find_class_named(classes, "Lcom/facebook/redextest/I_3;"), nullptr);
}
