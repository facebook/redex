/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <vector>

#include "SparseSetAbstractDomain.h"

// modified from HashedSetAbstractDomainTest
// since basic operation should have the same result

using Domain = SparseSetAbstractDomain;

TEST(SparseSetAbstractDomainTest, latticeOperations) {
  Domain e1(16);
  Domain e2(16);
  Domain e3(16);
  e1.add(1);
  e2.add(1);
  e2.add(2);
  e2.add(3);
  e3.add(2);
  e3.add(3);
  e3.add(4);
  EXPECT_THAT(e1.elements().vals(), ::testing::UnorderedElementsAre(1));
  EXPECT_THAT(e2.elements().vals(), ::testing::UnorderedElementsAre(1, 2, 3));
  EXPECT_THAT(e3.elements().vals(), ::testing::UnorderedElementsAre(2, 3, 4));
  e3.add(4);
  EXPECT_THAT(e3.elements().vals(), ::testing::UnorderedElementsAre(2, 3, 4));

  std::ostringstream out;
  out << e1;
  EXPECT_EQ("[#1]{1}", out.str());

  EXPECT_TRUE(Domain::bottom().leq(Domain::top()));
  EXPECT_FALSE(Domain::top().leq(Domain::bottom()));
  EXPECT_FALSE(e2.is_top());
  EXPECT_FALSE(e2.is_bottom());

  Domain e4(16);
  e4.add(2);
  e4.add(3);
  e4.add(1);
  EXPECT_TRUE(e1.leq(e2));
  EXPECT_FALSE(e1.leq(e3));
  EXPECT_TRUE(e2.equals(e4));
  EXPECT_FALSE(e2.equals(e3));

  EXPECT_THAT(e2.join(e3).elements().vals(),
              ::testing::UnorderedElementsAre(1, 2, 3, 4));
  EXPECT_THAT(e2.elements().vals(), ::testing::UnorderedElementsAre(1, 2, 3));
  EXPECT_TRUE(e1.join(e2).equals(e2));
  EXPECT_TRUE(e2.join(Domain::bottom()).equals(e2));
  EXPECT_TRUE(e2.join(Domain::top()).is_top());
  EXPECT_TRUE(e1.widening(e2).equals(e2));

  EXPECT_THAT(e2.meet(e3).elements().vals(),
              ::testing::UnorderedElementsAre(2, 3));
  EXPECT_TRUE(e1.meet(e2).equals(e1));
  EXPECT_TRUE(e2.meet(Domain::bottom()).is_bottom());
  EXPECT_TRUE(e2.meet(Domain::top()).equals(e2));
  EXPECT_FALSE(e1.meet(e3).is_bottom());
  EXPECT_TRUE(e1.meet(e3).elements().vals().empty());
  EXPECT_TRUE(e1.narrowing(e2).equals(e1));

  EXPECT_TRUE(e2.contains(1));
  EXPECT_FALSE(e3.contains(1));

  // Making sure no side effect happened.
  EXPECT_THAT(e1.elements().vals(), ::testing::UnorderedElementsAre(1));
  EXPECT_THAT(e2.elements().vals(), ::testing::UnorderedElementsAre(1, 2, 3));
  EXPECT_THAT(e3.elements().vals(), ::testing::UnorderedElementsAre(2, 3, 4));
}

TEST(HashedSetAbstractDomainTest, destructiveOperations) {
  Domain e1(16);
  Domain e2(16);
  Domain e3(16);
  e1.add(1);
  e2.add(1);
  e2.add(2);
  e2.add(3);
  e3.add(2);
  e3.add(3);
  e3.add(4);

  e1.add(2);
  EXPECT_THAT(e1.elements().vals(), ::testing::UnorderedElementsAre(1, 2));
  e1.add(1);
  e1.add(3);
  EXPECT_TRUE(e1.equals(e2));
  e1.add(1);
  e1.add(2);
  EXPECT_TRUE(e1.equals(e2));
  EXPECT_FALSE(e1.contains(18));
  EXPECT_FALSE(e1.contains(4));

  e1.remove(2);
  EXPECT_THAT(e1.elements().vals(), ::testing::UnorderedElementsAre(1, 3));
  e1.remove(4);
  EXPECT_THAT(e1.elements().vals(), ::testing::UnorderedElementsAre(1, 3));
  e1.remove(1);
  e1.remove(5);
  EXPECT_THAT(e1.elements().vals(), ::testing::UnorderedElementsAre(3));
  e1.remove(1);
  e1.remove(3);
  EXPECT_TRUE(e1.elements().vals().empty());

  e1.join_with(e2);
  EXPECT_THAT(e1.elements().vals(), ::testing::UnorderedElementsAre(1, 2, 3));
  e1.join_with(Domain::bottom());
  EXPECT_TRUE(e1.equals(e2));
  e1.join_with(Domain::top());
  EXPECT_TRUE(e1.is_top());

  e1 = Domain(16);
  e1.add(1);
  Domain e4(16);
  e4.add(2);
  e4.add(3);
  e1.widen_with(e4);
  EXPECT_TRUE(e1.equals(e2));

  e1 = Domain(16);
  e1.add(1);
  e2.meet_with(e3);
  EXPECT_THAT(e2.elements().vals(), ::testing::UnorderedElementsAre(2, 3));
  e1.meet_with(e2);
  EXPECT_TRUE(e1.elements().vals().empty());
  e1.meet_with(Domain::top());
  EXPECT_THAT(e2.elements().vals(), ::testing::UnorderedElementsAre(2, 3));
  e1.meet_with(Domain::bottom());
  EXPECT_TRUE(e1.is_bottom());

  e1 = Domain(16);
  e1.add(1);
  Domain e5(16);
  e5.add(1);
  e5.add(2);
  e1.narrow_with(e5);
  EXPECT_THAT(e1.elements().vals(), ::testing::UnorderedElementsAre(1));

  EXPECT_FALSE(e2.is_top());
  e1.set_to_top();
  EXPECT_TRUE(e1.is_top());
  e1.set_to_bottom();
  EXPECT_TRUE(e1.is_bottom());
  EXPECT_FALSE(e2.is_bottom());
  e2.set_to_bottom();
  EXPECT_TRUE(e2.is_bottom());

  e1 = Domain(16);
  e1.add(1);
  e1.add(2);
  e1.add(3);
  e1.add(4);
  e2 = e1;
  EXPECT_TRUE(e1.equals(e2));
  EXPECT_TRUE(e2.equals(e1));
  EXPECT_FALSE(e2.is_bottom());
  EXPECT_THAT(e2.elements().vals(),
              ::testing::UnorderedElementsAre(1, 2, 3, 4));
}
