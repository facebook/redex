/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PatriciaTreeMapAbstractEnvironment.h"

#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits>
#include <random>
#include <sstream>

#include "HashedAbstractEnvironment.h"
#include "HashedSetAbstractDomain.h"

using namespace sparta;

using Domain = HashedSetAbstractDomain<std::string>;
using Environment = PatriciaTreeMapAbstractEnvironment<uint32_t, Domain>;

class PatriciaTreeMapAbstractEnvironmentTest : public ::testing::Test {
 protected:
  PatriciaTreeMapAbstractEnvironmentTest()
      : m_rd_device(),
        m_generator(m_rd_device()),
        m_size_dist(0, 50),
        m_elem_dist(0, std::numeric_limits<uint32_t>::max()) {}

  Environment generate_random_environment() {
    Environment env;
    size_t size = m_size_dist(m_generator);
    for (size_t i = 0; i < size; ++i) {
      auto rnd = m_elem_dist(m_generator);
      auto rnd_string = std::to_string(m_elem_dist(m_generator));
      env.set(rnd, Domain({rnd_string}));
    }
    return env;
  }

  std::random_device m_rd_device;
  std::mt19937 m_generator;
  std::uniform_int_distribution<uint32_t> m_size_dist;
  std::uniform_int_distribution<uint32_t> m_elem_dist;
};

static HashedAbstractEnvironment<uint32_t, Domain> hae_from_ptae(
    const Environment& env) {
  HashedAbstractEnvironment<uint32_t, Domain> hae;
  if (env.is_value()) {
    for (const auto& pair : env.bindings()) {
      hae.set(pair.first, pair.second);
    }
  } else if (env.is_top()) {
    hae.set_to_top();
  } else {
    hae.set_to_bottom();
  }
  return hae;
}

TEST_F(PatriciaTreeMapAbstractEnvironmentTest, latticeOperations) {
  Environment e1({{1, Domain({"a", "b"})},
                  {2, Domain("c")},
                  {3, Domain({"d", "e", "f"})},
                  {4, Domain({"a", "f"})}});
  Environment e2({{0, Domain({"c", "f"})},
                  {2, Domain({"c", "d"})},
                  {3, Domain({"d", "e", "g", "h"})}});

  EXPECT_EQ(4, e1.size());
  EXPECT_EQ(3, e2.size());

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
  EXPECT_THAT(join.get(2).elements(),
              ::testing::UnorderedElementsAre("c", "d"));
  EXPECT_THAT(join.get(3).elements(),
              ::testing::UnorderedElementsAre("d", "e", "f", "g", "h"));
  EXPECT_TRUE(join.equals(e1.widening(e2)));

  EXPECT_TRUE(e1.join(Environment::top()).is_top());
  EXPECT_TRUE(e1.join(Environment::bottom()).equals(e1));

  Environment meet = e1.meet(e2);
  EXPECT_TRUE(meet.leq(e1));
  EXPECT_TRUE(meet.leq(e2));
  EXPECT_EQ(5, meet.size());
  EXPECT_THAT(meet.get(0).elements(),
              ::testing::UnorderedElementsAre("c", "f"));
  EXPECT_THAT(meet.get(1).elements(),
              ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(meet.get(2).elements(), ::testing::ElementsAre("c"));
  EXPECT_THAT(meet.get(3).elements(),
              ::testing::UnorderedElementsAre("d", "e"));
  EXPECT_THAT(meet.get(4).elements(),
              ::testing::UnorderedElementsAre("a", "f"));
  EXPECT_TRUE(meet.equals(e1.narrowing(e2)));

  EXPECT_TRUE(e1.meet(Environment::bottom()).is_bottom());
  EXPECT_TRUE(e1.meet(Environment::top()).equals(e1));

  Environment s1({{7, Domain({"a", "b"})}});
  Environment s2({{7, Domain({"a", "b", "c"})}});
  Environment s3({{4, Domain({"a", "b", "c"})}});
  EXPECT_TRUE(s1.leq(s2));
  EXPECT_FALSE(s2.leq(s1));
  EXPECT_FALSE(s1.leq(s3));
  EXPECT_FALSE(s2.leq(s3));
  EXPECT_FALSE(s3.leq(s2));
}

TEST_F(PatriciaTreeMapAbstractEnvironmentTest, destructiveOperations) {
  Environment e1({{1, Domain({"a", "b"})}});
  Environment e2({{2, Domain({"c", "d"})}, {3, Domain({"g", "h"})}});

  e1.set(2, Domain({"c", "f"})).set(4, Domain({"e", "f", "g"}));
  EXPECT_EQ(3, e1.size());
  EXPECT_THAT(e1.get(1).elements(), ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(e1.get(2).elements(), ::testing::UnorderedElementsAre("c", "f"));
  EXPECT_THAT(e1.get(4).elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Environment join = e1;
  join.join_with(e2);
  EXPECT_EQ(1, join.size()) << join;
  EXPECT_THAT(join.get(2).elements(),
              ::testing::UnorderedElementsAre("c", "d", "f"));

  Environment widening = e1;
  widening.widen_with(e2);
  EXPECT_TRUE(widening.equals(join));

  Environment meet = e1;
  meet.meet_with(e2);
  EXPECT_EQ(4, meet.size());
  EXPECT_THAT(meet.get(1).elements(),
              ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(meet.get(2).elements(), ::testing::ElementsAre("c"));
  EXPECT_THAT(meet.get(3).elements(),
              ::testing::UnorderedElementsAre("g", "h"));
  EXPECT_THAT(meet.get(4).elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Environment narrowing = e1;
  narrowing.narrow_with(e2);
  EXPECT_TRUE(narrowing.equals(meet));

  auto add_e = [](const Domain& s) {
    auto copy = s;
    copy.add("e");
    return copy;
  };
  e1.update(1, add_e).update(2, add_e);
  EXPECT_EQ(3, e1.size());
  EXPECT_THAT(e1.get(1).elements(),
              ::testing::UnorderedElementsAre("a", "b", "e"));
  EXPECT_THAT(e1.get(2).elements(),
              ::testing::UnorderedElementsAre("c", "e", "f"));
  EXPECT_THAT(e1.get(4).elements(),
              ::testing::UnorderedElementsAre("e", "f", "g"));

  Environment e3 = e2;
  EXPECT_EQ(2, e3.size());
  e3.update(1, add_e).update(2, add_e);
  EXPECT_EQ(2, e3.size());
  EXPECT_THAT(e3.get(2).elements(),
              ::testing::UnorderedElementsAre("c", "d", "e"));
  EXPECT_THAT(e3.get(3).elements(), ::testing::UnorderedElementsAre("g", "h"));

  auto make_bottom = [](const Domain&) { return Domain::bottom(); };
  Environment e4 = e2;
  e4.update(1, make_bottom);
  EXPECT_TRUE(e4.is_bottom());
  int counter = 0;
  auto make_e = [&counter](const Domain&) {
    ++counter;
    return Domain({"e"});
  };
  e4.update(1, make_e).update(2, make_e);
  EXPECT_TRUE(e4.is_bottom());
  // Since e4 is Bottom, make_e should have never been called.
  EXPECT_EQ(0, counter);

  auto refine_de = [](const Domain& s) {
    auto copy = s;
    copy.meet_with(Domain({"d", "e"}));
    return copy;
  };
  EXPECT_EQ(2, e2.size());
  e2.update(1, refine_de).update(2, refine_de);
  EXPECT_EQ(3, e2.size());
  EXPECT_THAT(e2.get(1).elements(), ::testing::UnorderedElementsAre("d", "e"));
  EXPECT_THAT(e2.get(2).elements(), ::testing::ElementsAre("d"));
  EXPECT_THAT(e2.get(3).elements(), ::testing::UnorderedElementsAre("g", "h"));
}

TEST_F(PatriciaTreeMapAbstractEnvironmentTest, robustness) {
  for (size_t k = 0; k < 10; ++k) {
    Environment e1 = this->generate_random_environment();
    Environment e2 = this->generate_random_environment();

    auto ref_meet = hae_from_ptae(e1);
    ref_meet.meet_with(hae_from_ptae(e2));
    auto meet = e1;
    meet.meet_with(e2);
    EXPECT_EQ(hae_from_ptae(meet), ref_meet);
    EXPECT_TRUE(meet.leq(e1));
    EXPECT_TRUE(meet.leq(e2));

    auto ref_join = hae_from_ptae(e1);
    ref_join.join_with(hae_from_ptae(e2));
    auto join = e1;
    join.join_with(e2);
    EXPECT_EQ(hae_from_ptae(join), ref_join);
    EXPECT_TRUE(e1.leq(join));
    EXPECT_TRUE(e2.leq(join));
  }
}

TEST_F(PatriciaTreeMapAbstractEnvironmentTest, whiteBox) {
  // The algorithms are designed in such a way that Patricia trees that are left
  // unchanged by an operation are not reconstructed (i.e., the result of an
  // operation shares structure with the operands whenever possible). This is
  // what we check here.
  Environment e({{1, Domain({"a"})}});
  const auto& before = e.bindings();
  e.update(1, [](const Domain& x) { return Domain({"a"}); });
  EXPECT_TRUE(e.bindings().reference_equals(before));
  e.meet_with(e);
  EXPECT_TRUE(e.bindings().reference_equals(before));
  e.join_with(e);
  EXPECT_TRUE(e.bindings().reference_equals(before));
}

TEST_F(PatriciaTreeMapAbstractEnvironmentTest, erase_all_matching) {
  Environment e1({{1, Domain({"a", "b"})}});
  bool any_changes = e1.erase_all_matching(0);
  EXPECT_FALSE(any_changes);
  any_changes = e1.erase_all_matching(1);
  EXPECT_TRUE(any_changes);
  EXPECT_TRUE(e1.is_top());
}

TEST_F(PatriciaTreeMapAbstractEnvironmentTest, map) {
  Environment e1({{1, Domain({"a", "b"})}});
  bool any_changes = e1.map([](Domain d) { return d; });
  EXPECT_FALSE(any_changes);

  any_changes = e1.map([](Domain d) { return Domain::top(); });
  EXPECT_TRUE(any_changes);
  EXPECT_TRUE(e1.is_top());
}

TEST_F(PatriciaTreeMapAbstractEnvironmentTest, prettyPrinting) {
  using StringEnvironment =
      PatriciaTreeMapAbstractEnvironment<std::string*, Domain>;
  std::string a = "a";
  StringEnvironment e({{&a, Domain("A")}});

  std::ostringstream out;
  out << e.bindings();
  EXPECT_EQ("{a -> [#1]{A}}", out.str());
}
