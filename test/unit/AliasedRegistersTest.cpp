/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "AliasedRegisters.h"
#include <boost/optional.hpp>
#include <unordered_map>

using namespace aliased_registers;
using namespace sparta;

Value zero = Value::create_register(0);
Value one = Value::create_register(1);
Value two = Value::create_register(2);
Value three = Value::create_register(3);
Value four = Value::create_register(4);

Value int_one_lit = Value::create_literal(1, constant_uses::TypeDemand::Int);

TEST(AliasedRegistersTest, identity) {
  AliasedRegisters a;
  EXPECT_TRUE(a.are_aliases(zero, zero));
  EXPECT_TRUE(a.are_aliases(one, one));
}

TEST(AliasedRegistersTest, simpleMake) {
  AliasedRegisters a;

  a.move(zero, one);

  EXPECT_TRUE(a.are_aliases(zero, zero));
  EXPECT_TRUE(a.are_aliases(zero, one));
  EXPECT_TRUE(a.are_aliases(one, one));

  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_FALSE(a.are_aliases(one, two));
}

TEST(AliasedRegistersTest, makeBreakLow) {
  AliasedRegisters a;

  a.move(zero, one);
  EXPECT_TRUE(a.are_aliases(zero, one));

  a.break_alias(zero);
  EXPECT_FALSE(a.are_aliases(zero, one));
}

TEST(AliasedRegistersTest, makeBreakHigh) {
  AliasedRegisters a;

  a.move(zero, one);
  EXPECT_TRUE(a.are_aliases(zero, one));

  a.break_alias(one);
  EXPECT_FALSE(a.are_aliases(zero, one));
}

TEST(AliasedRegistersTest, transitiveBreakFirst) {
  AliasedRegisters a;

  a.move(zero, one);
  a.move(two, one);
  EXPECT_TRUE(a.are_aliases(zero, two));

  a.break_alias(zero);
  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_TRUE(a.are_aliases(one, two));
}

TEST(AliasedRegistersTest, transitiveBreakMiddle) {
  AliasedRegisters a;

  a.move(zero, one);
  a.move(two, one);
  EXPECT_TRUE(a.are_aliases(zero, two));

  a.break_alias(one);
  EXPECT_TRUE(a.are_aliases(zero, two));
}

TEST(AliasedRegistersTest, transitiveBreakEnd) {
  AliasedRegisters a;

  a.move(zero, one);
  a.move(two, one);
  EXPECT_TRUE(a.are_aliases(zero, two));

  a.break_alias(two);
  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_TRUE(a.are_aliases(zero, one));
}

TEST(AliasedRegistersTest, transitiveTwoStep) {
  AliasedRegisters a;

  a.move(zero, one);
  a.move(two, one);
  a.move(three, two);

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

  a.move(zero, one);
  a.move(two, one);
  a.move(three, two);
  a.move(three, zero);

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

TEST(AliasedRegistersTest, getRepresentative) {
  AliasedRegisters a;
  a.move(zero, one);
  reg_t zero_rep = a.get_representative(zero);
  reg_t one_rep = a.get_representative(one);
  EXPECT_EQ(1, zero_rep);
  EXPECT_EQ(1, one_rep);
}

TEST(AliasedRegistersTest, getRepresentativeTwoLinks) {
  AliasedRegisters a;
  a.move(zero, one);
  a.move(two, zero);
  reg_t zero_rep = a.get_representative(zero);
  reg_t one_rep = a.get_representative(one);
  reg_t two_rep = a.get_representative(two);
  EXPECT_EQ(1, zero_rep);
  EXPECT_EQ(1, one_rep);
  EXPECT_EQ(1, two_rep);
}

TEST(AliasedRegistersTest, breakLineGraph) {
  AliasedRegisters a;
  a.move(zero, one);
  a.move(two, one);
  a.break_alias(one);
  EXPECT_TRUE(a.are_aliases(zero, two));

  a.clear();
  a.move(one, two);
  a.move(zero, one);
  a.break_alias(one);
  EXPECT_TRUE(a.are_aliases(zero, two));
  EXPECT_TRUE(a.are_aliases(two, zero));
  EXPECT_FALSE(a.are_aliases(one, two));
  EXPECT_FALSE(a.are_aliases(one, zero));
}

TEST(AliasedRegistersTest, getRepresentativeNone) {
  AliasedRegisters a;
  reg_t zero_rep = a.get_representative(zero);
  EXPECT_EQ(0, zero_rep);
}

TEST(AliasedRegistersTest, getRepresentativeTwoComponents) {
  AliasedRegisters a;
  a.move(zero, one);
  a.move(two, three);

  reg_t zero_rep = a.get_representative(zero);
  reg_t one_rep = a.get_representative(one);
  EXPECT_EQ(1, zero_rep);
  EXPECT_EQ(1, one_rep);

  reg_t two_rep = a.get_representative(two);
  reg_t three_rep = a.get_representative(three);
  EXPECT_EQ(3, two_rep);
  EXPECT_EQ(3, three_rep);
}

TEST(AliasedRegistersTest, getRepresentativeNoLits) {
  AliasedRegisters a;
  a.move(two, int_one_lit);
  auto two_rep = a.get_representative(two);
  EXPECT_EQ(2, two_rep);
}

TEST(AliasedRegistersTest, AbstractValueLeq) {
  AliasedRegisters a;
  AliasedRegisters b;
  EXPECT_TRUE(a.leq(b));
  EXPECT_TRUE(b.leq(a));

  a.move(zero, one);
  b.move(zero, one);

  EXPECT_TRUE(a.leq(b));

  b.move(two, zero);
  EXPECT_FALSE(a.leq(b));
  EXPECT_TRUE(b.leq(a));
}

TEST(AliasedRegistersTest, AbstractValueLeqAndNotEqual) {
  AliasedRegisters a;
  AliasedRegisters b;

  a.move(zero, one);
  b.move(two, three);

  EXPECT_FALSE(a.leq(b));
  EXPECT_FALSE(b.leq(a));
  EXPECT_FALSE(a.equals(b));
  EXPECT_FALSE(b.equals(a));
}

TEST(AliasedRegistersTest, AbstractValueEquals) {
  AliasedRegisters a;
  AliasedRegisters b;
  EXPECT_TRUE(a.equals(b));
  EXPECT_TRUE(b.equals(a));

  a.move(zero, one);
  b.move(zero, one);

  EXPECT_TRUE(a.equals(b));
  EXPECT_TRUE(b.equals(a));

  b.move(two, zero);
  EXPECT_FALSE(a.equals(b));
  EXPECT_FALSE(b.equals(a));
}

TEST(AliasedRegistersTest, AbstractValueEqualsAndClear) {
  AliasedRegisters a;
  AliasedRegisters b;
  EXPECT_TRUE(a.equals(b));

  a.move(zero, one);
  b.move(zero, one);

  EXPECT_TRUE(a.equals(b));

  b.clear();
  EXPECT_TRUE(a.equals(a));
  EXPECT_TRUE(b.equals(b));
  EXPECT_FALSE(a.equals(b));
}

TEST(AliasedRegistersTest, AbstractValueJoinNone) {
  AliasedRegisters a;
  AliasedRegisters b;

  a.move(zero, one);
  b.move(one, two);

  a.join_with(b);

  EXPECT_FALSE(a.are_aliases(zero, one));
  EXPECT_FALSE(a.are_aliases(one, two));
  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_FALSE(a.are_aliases(zero, three));
}

TEST(AliasedRegistersTest, AbstractValueJoinSome) {
  AliasedRegisters a;
  AliasedRegisters b;

  a.move(zero, one);
  b.move(zero, one);
  b.move(two, one);

  a.join_with(b);

  EXPECT_TRUE(a.are_aliases(zero, one));
  EXPECT_FALSE(a.are_aliases(one, two));
  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_FALSE(a.are_aliases(zero, three));

  EXPECT_TRUE(b.are_aliases(zero, one));
  EXPECT_TRUE(b.are_aliases(one, two));
  EXPECT_TRUE(b.are_aliases(zero, two));
  EXPECT_FALSE(b.are_aliases(zero, three));
}

TEST(AliasedRegistersTest, AbstractValueJoin) {
  AliasedRegisters a;
  AliasedRegisters b;

  a.move(zero, one);
  a.move(two, zero);
  a.move(three, zero);

  b.move(four, one);
  b.move(two, four);
  b.move(three, four);

  a.join_with(b);

  EXPECT_TRUE(a.are_aliases(one, two));
  EXPECT_TRUE(a.are_aliases(one, three));
  EXPECT_TRUE(a.are_aliases(two, three));

  EXPECT_FALSE(a.are_aliases(zero, one));
  EXPECT_FALSE(a.are_aliases(zero, two));
  EXPECT_FALSE(a.are_aliases(zero, three));
  EXPECT_FALSE(a.are_aliases(zero, four));

  EXPECT_FALSE(a.are_aliases(four, one));
  EXPECT_FALSE(a.are_aliases(four, two));
  EXPECT_FALSE(a.are_aliases(four, three));
}

TEST(AliasedRegistersTest, CopyOnWriteDomain) {
  AliasDomain x(AbstractValueKind::Top);
  AliasDomain y = x; // take a reference

  x.update([](AliasedRegisters& a) { // cause a change in x. Forcing a copy
    a.move(zero, one);
  });

  y.update([](AliasedRegisters& a) { // make sure y isn't still referencing x
    EXPECT_FALSE(a.are_aliases(zero, one));
  });
}
