/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HashedAbstractPartition.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include "HashedSetAbstractDomain.h"

using namespace sparta;

using Domain = HashedSetAbstractDomain<std::string>;

using Partition = HashedAbstractPartition<std::string, Domain>;

TEST(HashedAbstractPartitionTest, latticeOperations) {
  Partition p1({{"v1", Domain({"a", "b"})},
                {"v2", Domain("c")},
                {"v3", Domain({"d", "e", "f"})},
                {"v4", Domain({"a", "f"})}});
  Partition p2({{"v0", Domain({"c", "f"})},
                {"v2", Domain({"c", "d"})},
                {"v3", Domain({"d", "e", "g", "h"})}});
  EXPECT_EQ(4, p1.size());
  EXPECT_EQ(3, p2.size());

  EXPECT_TRUE(Partition::top().leq(Partition::top()));
  EXPECT_FALSE(Partition::top().leq(Partition::bottom()));
  EXPECT_TRUE(Partition::bottom().leq(Partition::top()));
  EXPECT_TRUE(Partition::bottom().leq(Partition::bottom()));

  EXPECT_TRUE(Partition::bottom().leq(p1));
  EXPECT_FALSE(p1.leq(Partition::bottom()));
  EXPECT_FALSE(Partition::top().leq(p1));
  EXPECT_TRUE(p1.leq(Partition::top()));
  EXPECT_FALSE(p1.leq(p2));
  EXPECT_FALSE(p2.leq(p1));

  EXPECT_TRUE(p1.equals(p1));
  EXPECT_FALSE(p1.equals(p2));
  EXPECT_TRUE(Partition::bottom().equals(Partition()));
  EXPECT_TRUE(Partition::bottom().equals(Partition::bottom()));
  EXPECT_TRUE(Partition::top().equals(Partition::top()));
  EXPECT_FALSE(Partition::bottom().equals(Partition::top()));

  Partition join = p1.join(p2);
  EXPECT_TRUE(p1.leq(join));
  EXPECT_TRUE(p2.leq(join));
  EXPECT_EQ(5, join.size());
  EXPECT_EQ(join.get("v0").elements(), p2.get("v0").elements());
  EXPECT_EQ(join.get("v1").elements(), p1.get("v1").elements());
  EXPECT_THAT(join.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "d"));
  EXPECT_THAT(join.get("v3").elements(),
              ::testing::UnorderedElementsAre("d", "e", "f", "g", "h"));
  EXPECT_EQ(join.get("v4").elements(), p1.get("v4").elements());
  EXPECT_TRUE(join.equals(p1.widening(p2)));

  EXPECT_TRUE(p1.join(Partition::top()).is_top());
  EXPECT_TRUE(p1.join(Partition::bottom()).equals(p1));

  Partition meet = p1.meet(p2);
  EXPECT_TRUE(meet.leq(p1));
  EXPECT_TRUE(meet.leq(p2));
  EXPECT_EQ(2, meet.size());
  EXPECT_THAT(meet.get("v2").elements(), ::testing::ElementsAre("c"));
  EXPECT_THAT(meet.get("v3").elements(),
              ::testing::UnorderedElementsAre("d", "e"));
  EXPECT_EQ(meet, p1.narrowing(p2));

  EXPECT_TRUE(p1.meet(Partition::bottom()).is_bottom());
  EXPECT_EQ(p1.meet(Partition::top()), p1);
}

TEST(HashedAbstractPartitionTest, destructiveOperations) {
  Partition p1({{"v1", Domain({"a", "b"})}});
  Partition p2({{"v2", Domain({"c", "d"})}, {"v3", Domain({"g", "h"})}});

  p1.set("v2", Domain({"c", "f"})).set("v4", Domain({"e", "f", "g"}));
  EXPECT_EQ(3, p1.size());
  EXPECT_THAT(p1.get("v1").elements(),
              ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(p1.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "f"));
  EXPECT_THAT(p1.get("v4").elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Partition join = p1;
  join.join_with(p2);
  EXPECT_EQ(4, join.size());
  EXPECT_EQ(join.get("v1").elements(), p1.get("v1").elements());
  EXPECT_EQ(join.get("v4").elements(), p1.get("v4").elements());
  EXPECT_THAT(join.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "d", "f"));
  EXPECT_EQ(join.get("v3").elements(), p2.get("v3").elements());

  Partition widening = p1;
  widening.widen_with(p2);
  EXPECT_TRUE(widening.equals(join));

  Partition meet = p1;
  meet.meet_with(p2);
  EXPECT_EQ(1, meet.size());
  EXPECT_THAT(meet.get("v2").elements(), ::testing::ElementsAre("c"));

  Partition narrowing = p1;
  narrowing.narrow_with(p2);
  EXPECT_TRUE(narrowing.equals(meet));

  auto add_e = [](Domain* s) { s->add("e"); };
  p1.update("v1", add_e).update("v2", add_e);
  EXPECT_EQ(3, p1.size());
  EXPECT_THAT(p1.get("v1").elements(),
              ::testing::UnorderedElementsAre("a", "b", "e"));
  EXPECT_THAT(p1.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "e", "f"));
  EXPECT_THAT(p1.get("v4").elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Partition p3 = p2;
  EXPECT_EQ(2, p3.size());
  p3.update("v1", add_e).update("v2", add_e);
  EXPECT_EQ(2, p3.size());
  EXPECT_THAT(p3.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "d", "e"));
  EXPECT_THAT(p3.get("v3").elements(),
              ::testing::UnorderedElementsAre("g", "h"));

  auto make_bottom = [](Domain* s) { s->set_to_bottom(); };
  Partition p4 = p2;
  p4.update("v2", make_bottom);
  EXPECT_FALSE(p4.is_bottom());
  EXPECT_EQ(1, p4.size());

  auto refine_de = [](Domain* s) { s->meet_with(Domain({"d", "e"})); };
  EXPECT_EQ(2, p2.size());
  p2.update("v1", refine_de).update("v2", refine_de);
  EXPECT_EQ(2, p2.size());
  EXPECT_TRUE(p2.get("v1").is_bottom());
  EXPECT_THAT(p2.get("v2").elements(), ::testing::ElementsAre("d"));
  EXPECT_THAT(p2.get("v3").elements(),
              ::testing::UnorderedElementsAre("g", "h"));

  Partition p5({{"v0", Domain({"c", "d"})},
                {"v2", Domain::bottom()},
                {"v3", Domain({"a", "f", "g"})}});

  EXPECT_EQ(2, p5.size());
  p5.set("v0", Domain::bottom());
  p5.set("v3", Domain::bottom());
  EXPECT_TRUE(p5.is_bottom());
  EXPECT_EQ(Partition::bottom(), p5);
  EXPECT_TRUE(p5.get("v4").is_bottom());

  Partition p6{Partition::top()};
  EXPECT_TRUE(p6.get("v0").is_top());

  // All operations on Top are no-ops.
  p6.set("v1", Domain::bottom());
  EXPECT_TRUE(p6.get("v1").is_top());
  EXPECT_TRUE(p6.is_top());

  p6.update("v1", make_bottom);
  EXPECT_TRUE(p6.get("v1").is_top());
  EXPECT_TRUE(p6.is_top());
}
