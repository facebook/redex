/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>

#include "Show.h"
#include "verify/VerifyUtil.h"

using namespace testing;

TEST_F(PostVerify, VerifyKeptAndRemoved) {
  EXPECT_EQ(find_class_named(classes, "Lcom/redex/Unused;"), nullptr)
      << "Should remove type Unused";
  EXPECT_NE(find_class_named(classes, "Lcom/redex/Two;"), nullptr)
      << "Should not remove Two! It has a code reference.";
  EXPECT_NE(find_class_named(classes, "Lcom/redex/One;"), nullptr)
      << "Should not remove One! Otherwise we'll have a torn enum, and that's "
         "bad :p";
  EXPECT_NE(find_class_named(classes, "Lcom/redex/Zero;"), nullptr)
      << "Should not remove Zero!";
  EXPECT_NE(find_class_named(classes, "Lcom/redex/Funny;"), nullptr)
      << "Should not remove Funny! It is referenced from a static field.";
  EXPECT_NE(find_class_named(classes, "Lcom/redex/VeryFunny;"), nullptr)
      << "Should not remove VeryFunny! It is referenced from a static field.";
}
