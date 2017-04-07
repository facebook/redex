/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include <unordered_map>
#include "AliasedRegisters.h"

RegisterValue zero{(uint16_t) 0};
RegisterValue one{(uint16_t) 1};
RegisterValue two{(uint16_t) 2};
RegisterValue three{(uint16_t) 3};

TEST(AliasedRegistersTest, identity) {
  AliasedRegisters a;
  EXPECT_TRUE(a.are_aliases(zero, zero));
  EXPECT_TRUE(a.are_aliases(one, one));
}

TEST(AliasedRegistersTest, simpleMake) {
  AliasedRegisters a;

  a.make_aliased(zero, one);

  EXPECT_TRUE(a.are_aliases(zero, zero));
  EXPECT_TRUE(a.are_aliases(zero, one));
  EXPECT_TRUE(a.are_aliases(one, one));

  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_FALSE(a.are_aliases(one, two));
}

TEST(AliasedRegistersTest, makeBreakLow) {
  AliasedRegisters a;

  a.make_aliased(zero, one);
  EXPECT_TRUE(a.are_aliases(zero, one));

  a.break_alias(zero);
  EXPECT_FALSE(a.are_aliases(zero, one));
}

TEST(AliasedRegistersTest, makeBreakHigh) {
  AliasedRegisters a;

  a.make_aliased(zero, one);
  EXPECT_TRUE(a.are_aliases(zero, one));

  a.break_alias(one);
  EXPECT_FALSE(a.are_aliases(zero, one));
}

TEST(AliasedRegistersTest, transitiveBreakFirst) {
  AliasedRegisters a;

  a.make_aliased(zero, one);
  a.make_aliased(one, two);
  EXPECT_TRUE(a.are_aliases(zero, two));

  a.break_alias(zero);
  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_TRUE(a.are_aliases(one, two));
}

TEST(AliasedRegistersTest, transitiveBreakMiddle) {
  AliasedRegisters a;

  a.make_aliased(zero, one);
  a.make_aliased(one, two);
  EXPECT_TRUE(a.are_aliases(zero, two));

  a.break_alias(one);
  EXPECT_FALSE(a.are_aliases(zero, two));
}

TEST(AliasedRegistersTest, transitiveBreakEnd) {
  AliasedRegisters a;

  a.make_aliased(zero, one);
  a.make_aliased(one, two);
  EXPECT_TRUE(a.are_aliases(zero, two));

  a.break_alias(two);
  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_TRUE(a.are_aliases(zero, one));
}

TEST(AliasedRegistersTest, transitiveTwoStep) {
  AliasedRegisters a;

  a.make_aliased(zero, one);
  a.make_aliased(one, two);
  a.make_aliased(three, two);

  EXPECT_TRUE(a.are_aliases(zero, three));
  EXPECT_TRUE(a.are_aliases(zero, two));
  EXPECT_TRUE(a.are_aliases(zero, one));

  EXPECT_TRUE(a.are_aliases(one, zero));
  EXPECT_TRUE(a.are_aliases(one, two));
  EXPECT_TRUE(a.are_aliases(one, three));

  EXPECT_TRUE(a.are_aliases(two, zero));
  EXPECT_TRUE(a.are_aliases(two, one));
  EXPECT_TRUE(a.are_aliases(two, three));

  EXPECT_TRUE(a.are_aliases(three, zero));
  EXPECT_TRUE(a.are_aliases(three, one));
  EXPECT_TRUE(a.are_aliases(three, two));

  a.break_alias(two);

  EXPECT_TRUE(a.are_aliases(zero, one));
  EXPECT_TRUE(a.are_aliases(one, zero));
}

TEST(AliasedRegistersTest, transitiveCycleBreak) {
  AliasedRegisters a;

  a.make_aliased(zero, one);
  a.make_aliased(one, two);
  a.make_aliased(three, two);
  a.make_aliased(zero, three);

  EXPECT_TRUE(a.are_aliases(zero, three));
  EXPECT_TRUE(a.are_aliases(zero, two));
  EXPECT_TRUE(a.are_aliases(zero, one));

  EXPECT_TRUE(a.are_aliases(one, zero));
  EXPECT_TRUE(a.are_aliases(one, two));
  EXPECT_TRUE(a.are_aliases(one, three));

  EXPECT_TRUE(a.are_aliases(two, zero));
  EXPECT_TRUE(a.are_aliases(two, one));
  EXPECT_TRUE(a.are_aliases(two, three));

  EXPECT_TRUE(a.are_aliases(three, zero));
  EXPECT_TRUE(a.are_aliases(three, one));
  EXPECT_TRUE(a.are_aliases(three, two));

  a.break_alias(two);

  EXPECT_TRUE(a.are_aliases(zero, one));
  EXPECT_TRUE(a.are_aliases(one, zero));

  EXPECT_TRUE(a.are_aliases(zero, three));
  EXPECT_TRUE(a.are_aliases(three, zero));

  EXPECT_TRUE(a.are_aliases(one, three));
  EXPECT_TRUE(a.are_aliases(three, one));
}
