/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <memory>

#include "DexClass.h"
#include "RedexTest.h"

class Mutf8CompareTest : public RedexTest {};

TEST_F(Mutf8CompareTest, empty) {
  auto s1 = DexString::make_string(";");
  auto s2 = DexString::make_string(";\300\200", 2);
  EXPECT_TRUE(compare_dexstrings(s1, s2));
  EXPECT_FALSE(compare_dexstrings(s2, s1));
}
