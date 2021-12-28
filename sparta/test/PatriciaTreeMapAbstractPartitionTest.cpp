/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PatriciaTreeMapAbstractPartition.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include "AbstractDomainPropertyTest.h"
#include "HashedSetAbstractDomain.h"

using namespace sparta;

using Domain = HashedSetAbstractDomain<std::string>;

using Partition = PatriciaTreeMapAbstractPartition<uint32_t, Domain>;

INSTANTIATE_TYPED_TEST_CASE_P(PatriciaTreeMapAbstractPartition,
                              AbstractDomainPropertyTest,
                              Partition);

template <>
std::vector<Partition>
AbstractDomainPropertyTest<Partition>::non_extremal_values() {
  Partition p1({{1, Domain({"a", "b"})},
                {2, Domain("c")},
                {3, Domain({"d", "e", "f"})},
                {4, Domain({"a", "f"})}});
  Partition p2({{0, Domain({"c", "f"})},
                {2, Domain({"c", "d"})},
                {3, Domain({"d", "e", "g", "h"})}});
  return {p1, p2};
}

TEST(PatriciaTreeMapAbstractPartitionTest, basicPartialOrders) {
  {
    Partition p1;
    EXPECT_TRUE(p1.leq(p1));
  }

  {
    Partition p1;
    Partition p2;
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_TRUE(p2.leq(p1));
  }

  {
    Partition p1({{1, Domain({"a"})}});
    Partition p2({{1, Domain({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_TRUE(p2.leq(p1));
  }

  {
    Partition p1({{2, Domain({"a"})}, {3, Domain({"a"})}});
    Partition p2({{2, Domain({"a"})}, {3, Domain({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_TRUE(p2.leq(p1));
  }

  {
    Partition p1;
    Partition p2({{1, Domain({"a"})}});
    Partition p3({{2, Domain({"a"})}, {3, Domain({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
    EXPECT_TRUE(p1.leq(p3));
    EXPECT_FALSE(p3.leq(p1));
  }

  {
    Partition p1({{1, Domain({"a"})}});
    Partition p2({{1, Domain({"a"})}, {2, Domain({"a"})}});
    Partition p3({{2, Domain({"a"})}, {3, Domain({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
    EXPECT_FALSE(p1.leq(p3));
    EXPECT_FALSE(p3.leq(p1));
  }

  {
    Partition p1;
    p1.set_to_bottom();
    p1.set(1, Domain({"a"}));
    p1.set(2, Domain({"a"}));
    Partition p2;
    p2.set_to_bottom();
    p2.set(1, Domain({"a"}));
    EXPECT_FALSE(p1.leq(p2));
    EXPECT_TRUE(p2.leq(p1));
  }

  {
    Partition p1({{1, Domain({"a"})}, {3, Domain({"a"})}});
    Partition p2({{1, Domain({"a"})}, {2, Domain({"a"})}, {3, Domain({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
  }

  {
    Partition p1({{1, Domain({"a"})}, {3, Domain({"b"})}});
    Partition p2({{1, Domain({"a"})}, {2, Domain({"a"})}, {3, Domain({"a"})}});
    EXPECT_FALSE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
  }

  {
    Partition p1({{1, Domain({"a"})}, {3, Domain({"b"})}});
    Partition p2(
        {{1, Domain({"a", "b"})}, {2, Domain({"a"})}, {3, Domain({"a", "b"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
  }
}

TEST(PatriciaTreeMapAbstractPartitionTest, latticeOperations) {
  Partition p1({{1, Domain({"a", "b"})},
                {2, Domain("c")},
                {3, Domain({"d", "e", "f"})},
                {4, Domain({"a", "f"})}});
  Partition p2({{0, Domain({"c", "f"})},
                {2, Domain({"c", "d"})},
                {3, Domain({"d", "e", "g", "h"})}});
  EXPECT_EQ(4, p1.size());
  EXPECT_EQ(3, p2.size());

  EXPECT_FALSE(p1.leq(p2));
  EXPECT_FALSE(p2.leq(p1));

  EXPECT_FALSE(p1.equals(p2));
  EXPECT_TRUE(Partition::bottom().equals(Partition()));

  Partition join = p1.join(p2);
  EXPECT_EQ(5, join.size());
  EXPECT_EQ(join.get(0).elements(), p2.get(0).elements());
  EXPECT_EQ(join.get(1).elements(), p1.get(1).elements());
  EXPECT_THAT(join.get(2).elements(),
              ::testing::UnorderedElementsAre("c", "d"));
  EXPECT_THAT(join.get(3).elements(),
              ::testing::UnorderedElementsAre("d", "e", "f", "g", "h"));
  EXPECT_EQ(join.get(4).elements(), p1.get(4).elements());
  EXPECT_TRUE(join.equals(p1.widening(p2)));

  Partition meet = p1.meet(p2);
  EXPECT_EQ(2, meet.size());
  EXPECT_THAT(meet.get(2).elements(), ::testing::ElementsAre("c"));
  EXPECT_THAT(meet.get(3).elements(),
              ::testing::UnorderedElementsAre("d", "e"));
  EXPECT_EQ(meet, p1.narrowing(p2));
}

TEST(PatriciaTreeMapAbstractPartitionTest, destructiveOperations) {
  Partition p1({{1, Domain({"a", "b"})}});
  Partition p2({{2, Domain({"c", "d"})}, {3, Domain({"g", "h"})}});

  p1.set(2, Domain({"c", "f"})).set(4, Domain({"e", "f", "g"}));
  EXPECT_EQ(3, p1.size());
  EXPECT_THAT(p1.get(1).elements(), ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(p1.get(2).elements(), ::testing::UnorderedElementsAre("c", "f"));
  EXPECT_THAT(p1.get(4).elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Partition join = p1;
  join.join_with(p2);
  EXPECT_EQ(4, join.size());
  EXPECT_EQ(join.get(1).elements(), p1.get(1).elements());
  EXPECT_EQ(join.get(4).elements(), p1.get(4).elements());
  EXPECT_THAT(join.get(2).elements(),
              ::testing::UnorderedElementsAre("c", "d", "f"));
  EXPECT_EQ(join.get(3).elements(), p2.get(3).elements());

  Partition widening = p1;
  widening.widen_with(p2);
  EXPECT_TRUE(widening.equals(join));

  Partition meet = p1;
  meet.meet_with(p2);
  EXPECT_EQ(1, meet.size());
  EXPECT_THAT(meet.get(2).elements(), ::testing::ElementsAre("c"));

  Partition narrowing = p1;
  narrowing.narrow_with(p2);
  EXPECT_TRUE(narrowing.equals(meet));

  auto add_e = [](const Domain& s) {
    auto copy = s;
    copy.add("e");
    return copy;
  };
  p1.update(1, add_e).update(2, add_e);
  EXPECT_EQ(3, p1.size());
  EXPECT_THAT(p1.get(1).elements(),
              ::testing::UnorderedElementsAre("a", "b", "e"));
  EXPECT_THAT(p1.get(2).elements(),
              ::testing::UnorderedElementsAre("c", "e", "f"));
  EXPECT_THAT(p1.get(4).elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Partition p3 = p2;
  EXPECT_EQ(2, p3.size());
  p3.update(1, add_e).update(2, add_e);
  EXPECT_EQ(2, p3.size());
  EXPECT_THAT(p3.get(2).elements(),
              ::testing::UnorderedElementsAre("c", "d", "e"));
  EXPECT_THAT(p3.get(3).elements(), ::testing::UnorderedElementsAre("g", "h"));

  auto make_bottom = [](const Domain&) { return Domain::bottom(); };
  Partition p4 = p2;
  p4.update(2, make_bottom);
  EXPECT_FALSE(p4.is_bottom());
  EXPECT_EQ(1, p4.size());

  auto refine_de = [](const Domain& s) { return s.meet(Domain({"d", "e"})); };
  EXPECT_EQ(2, p2.size());
  p2.update(1, refine_de).update(2, refine_de);
  EXPECT_EQ(2, p2.size());
  EXPECT_TRUE(p2.get(1).is_bottom());
  EXPECT_THAT(p2.get(2).elements(), ::testing::ElementsAre("d"));
  EXPECT_THAT(p2.get(3).elements(), ::testing::UnorderedElementsAre("g", "h"));

  Partition p5({{0, Domain({"c", "d"})},
                {2, Domain::bottom()},
                {3, Domain({"a", "f", "g"})}});

  EXPECT_EQ(2, p5.size());
  p5.set(0, Domain::bottom());
  p5.set(3, Domain::bottom());
  EXPECT_TRUE(p5.is_bottom());
  EXPECT_EQ(Partition::bottom(), p5);
  EXPECT_TRUE(p5.get(4).is_bottom());

  Partition p6{Partition::top()};
  EXPECT_TRUE(p6.get(0).is_top());

  // All operations on Top are no-ops.
  p6.set(1, Domain::bottom());
  EXPECT_TRUE(p6.get(1).is_top());
  EXPECT_TRUE(p6.is_top());

  p6.update(1, make_bottom);
  EXPECT_TRUE(p6.get(1).is_top());
  EXPECT_TRUE(p6.is_top());
}

TEST(PatriciaTreeMapAbstractPartitionTest, map) {
  Partition p1({{1, Domain({"a", "b"})}});
  bool any_changes = p1.map([](Domain d) { return d; });
  EXPECT_FALSE(any_changes);

  any_changes = p1.map([](Domain d) { return Domain::bottom(); });
  EXPECT_TRUE(any_changes);
  EXPECT_TRUE(p1.is_bottom());
}
