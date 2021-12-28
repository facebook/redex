/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HashedAbstractEnvironment.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include "HashedSetAbstractDomain.h"

using namespace sparta;

using Domain = HashedSetAbstractDomain<std::string>;

using Environment = HashedAbstractEnvironment<std::string, Domain>;

TEST(HashedAbstractEnvironmentTest, latticeOperations) {
  Environment e1({{"v1", Domain({"a", "b"})},
                  {"v2", Domain("c")},
                  {"v3", Domain({"d", "e", "f"})},
                  {"v4", Domain({"a", "f"})}});
  Environment e2({{"v0", Domain({"c", "f"})},
                  {"v2", Domain({"c", "d"})},
                  {"v3", Domain({"d", "e", "g", "h"})}});
  Environment e3({{"v0", Domain({"c", "d"})},
                  {"v2", Domain::bottom()},
                  {"v3", Domain({"a", "f", "g"})}});

  EXPECT_EQ(4, e1.size());
  EXPECT_EQ(3, e2.size());
  EXPECT_TRUE(e3.is_bottom());

  EXPECT_TRUE(Environment::bottom().leq(e1));
  EXPECT_FALSE(e1.leq(Environment::bottom()));
  EXPECT_FALSE(Environment::top().leq(e1));
  EXPECT_TRUE(e1.leq(Environment::top()));
  EXPECT_FALSE(e1.leq(e2));
  EXPECT_FALSE(e2.leq(e1));

  EXPECT_TRUE(e1.equals(e1));
  EXPECT_FALSE(e1.equals(e2));
  EXPECT_TRUE(Environment::bottom().equals(Environment::bottom()));
  EXPECT_TRUE(Environment::top().equals(Environment::top()));
  EXPECT_FALSE(Environment::bottom().equals(Environment::top()));

  Environment join = e1.join(e2);
  EXPECT_TRUE(e1.leq(join));
  EXPECT_TRUE(e2.leq(join));
  EXPECT_EQ(2, join.size());
  EXPECT_THAT(join.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "d"));
  EXPECT_THAT(join.get("v3").elements(),
              ::testing::UnorderedElementsAre("d", "e", "f", "g", "h"));
  EXPECT_TRUE(join.equals(e1.widening(e2)));

  EXPECT_TRUE(e1.join(Environment::top()).is_top());
  EXPECT_TRUE(e1.join(Environment::bottom()).equals(e1));

  Environment meet = e1.meet(e2);
  EXPECT_TRUE(meet.leq(e1));
  EXPECT_TRUE(meet.leq(e2));
  EXPECT_EQ(5, meet.size());
  EXPECT_THAT(meet.get("v0").elements(),
              ::testing::UnorderedElementsAre("c", "f"));
  EXPECT_THAT(meet.get("v1").elements(),
              ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(meet.get("v2").elements(), ::testing::ElementsAre("c"));
  EXPECT_THAT(meet.get("v3").elements(),
              ::testing::UnorderedElementsAre("d", "e"));
  EXPECT_THAT(meet.get("v4").elements(),
              ::testing::UnorderedElementsAre("a", "f"));
  EXPECT_TRUE(meet.equals(e1.narrowing(e2)));

  EXPECT_TRUE(e1.meet(Environment::bottom()).is_bottom());
  EXPECT_TRUE(e1.meet(Environment::top()).equals(e1));
}

TEST(HashedAbstractEnvironmentTest, destructiveOperations) {
  Environment e1({{"v1", Domain({"a", "b"})}});
  Environment e2({{"v2", Domain({"c", "d"})}, {"v3", Domain({"g", "h"})}});

  e1.set("v2", Domain({"c", "f"})).set("v4", Domain({"e", "f", "g"}));
  EXPECT_EQ(3, e1.size());
  EXPECT_THAT(e1.get("v1").elements(),
              ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(e1.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "f"));
  EXPECT_THAT(e1.get("v4").elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Environment join = e1;
  join.join_with(e2);
  EXPECT_EQ(1, join.size());
  EXPECT_THAT(join.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "d", "f"));

  Environment widening = e1;
  widening.widen_with(e2);
  EXPECT_TRUE(widening.equals(join));

  Environment meet = e1;
  meet.meet_with(e2);
  EXPECT_EQ(4, meet.size());
  EXPECT_THAT(meet.get("v1").elements(),
              ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(meet.get("v2").elements(), ::testing::ElementsAre("c"));
  EXPECT_THAT(meet.get("v3").elements(),
              ::testing::UnorderedElementsAre("g", "h"));
  EXPECT_THAT(meet.get("v4").elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Environment narrowing = e1;
  narrowing.narrow_with(e2);
  EXPECT_TRUE(narrowing.equals(meet));

  auto add_e = [](Domain* s) { s->add("e"); };
  e1.update("v1", add_e).update("v2", add_e);
  EXPECT_EQ(3, e1.size());
  EXPECT_THAT(e1.get("v1").elements(),
              ::testing::UnorderedElementsAre("a", "b", "e"));
  EXPECT_THAT(e1.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "e", "f"));
  EXPECT_THAT(e1.get("v4").elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Environment e3 = e2;
  EXPECT_EQ(2, e3.size());
  e3.update("v1", add_e).update("v2", add_e);
  EXPECT_EQ(2, e3.size());
  EXPECT_THAT(e3.get("v2").elements(),
              ::testing::UnorderedElementsAre("c", "d", "e"));
  EXPECT_THAT(e3.get("v3").elements(),
              ::testing::UnorderedElementsAre("g", "h"));

  auto make_bottom = [](Domain* s) { s->set_to_bottom(); };
  Environment e4 = e2;
  e4.update("v1", make_bottom);
  EXPECT_TRUE(e4.is_bottom());
  int counter = 0;
  auto make_e = [&counter](Domain* s) {
    ++counter;
    *s = Domain({"e"});
  };
  e4.update("v1", make_e).update("v2", make_e);
  EXPECT_TRUE(e4.is_bottom());
  // Since e4 is Bottom, make_e should have never been called.
  EXPECT_EQ(0, counter);

  auto refine_de = [](Domain* s) { s->meet_with(Domain({"d", "e"})); };
  EXPECT_EQ(2, e2.size());
  e2.update("v1", refine_de).update("v2", refine_de);
  EXPECT_EQ(3, e2.size());
  EXPECT_THAT(e2.get("v1").elements(),
              ::testing::UnorderedElementsAre("d", "e"));
  EXPECT_THAT(e2.get("v2").elements(), ::testing::ElementsAre("d"));
  EXPECT_THAT(e2.get("v3").elements(),
              ::testing::UnorderedElementsAre("g", "h"));
}
