/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <memory>
#include <gtest/gtest.h>

#include "DexClass.h"

TEST(Mutf8CompareTest, empty) {
  g_redex = new RedexContext();
  DexString* s1 = DexString::make_string(";");
  DexString* s2 = DexString::make_string(";\300\200", 2);
  EXPECT_TRUE(compare_dexstrings(s1, s2));
  EXPECT_FALSE(compare_dexstrings(s2, s1));
  delete g_redex;
}
