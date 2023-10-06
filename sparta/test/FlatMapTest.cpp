/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/FlatMap.h>
#include <sparta/HashedSetAbstractDomain.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <initializer_list>
#include <random>
#include <unordered_map>

#include <boost/concept/assert.hpp>
#include <boost/concept_check.hpp>

using namespace sparta;

using IntFlatMap = FlatMap<uint32_t, uint32_t>;
using UnorderedMap = std::unordered_map<uint32_t, uint32_t>;
using StringAbstractSet = HashedSetAbstractDomain<std::string>;

BOOST_CONCEPT_ASSERT((boost::ForwardContainer<IntFlatMap>));

class FlatMapTest : public ::testing::Test {
 protected:
  FlatMapTest()
      : m_rd_device(),
        m_generator(m_rd_device()),
        m_size_dist(0, 50),
        m_elem_dist(0, std::numeric_limits<uint32_t>::max()) {}

  UnorderedMap generate_random_unordered_map() {
    UnorderedMap map;
    size_t size = m_size_dist(m_generator);
    for (size_t i = 0; i < size; ++i) {
      uint32_t key = m_elem_dist(m_generator);
      uint32_t value = m_elem_dist(m_generator);
      map.insert_or_assign(key, value);
    }
    return map;
  }

  std::random_device m_rd_device;
  std::mt19937 m_generator;
  std::uniform_int_distribution<uint32_t> m_size_dist;
  std::uniform_int_distribution<uint32_t> m_elem_dist;
};

TEST_F(FlatMapTest, inserts) {
  constexpr uint32_t bigint = std::numeric_limits<uint32_t>::max();
  constexpr uint32_t default_value = 0;
  IntFlatMap m1;
  IntFlatMap empty_map;
  std::vector<std::pair<uint32_t, uint32_t>> pairs1 = {
      {0, 3}, {1, 2}, {bigint, 3}};

  for (const auto& p : pairs1) {
    m1.insert_or_assign(p.first, p.second);
  }
  EXPECT_EQ(3, m1.size());
  EXPECT_THAT(m1, ::testing::UnorderedElementsAreArray(pairs1));

  for (const auto& p : pairs1) {
    EXPECT_EQ(m1.at(p.first), p.second);
    EXPECT_EQ(empty_map.at(p.first), default_value);
  }

  m1.insert_or_assign(17, default_value);
  // default values are implicitly stored
  EXPECT_EQ(3, m1.size());
  EXPECT_EQ(m1.at(17), default_value);

  EXPECT_EQ(m1.at(1000000), default_value);
}

TEST_F(FlatMapTest, robustness) {
  for (size_t k = 0; k < 10; ++k) {
    UnorderedMap original_map = this->generate_random_unordered_map();

    IntFlatMap flat_map;
    for (auto [key, value] : original_map) {
      flat_map.insert_or_assign(key, value);
    }

    EXPECT_TRUE(flat_map.size() <= original_map.size());
    for (auto [key, value] : original_map) {
      EXPECT_EQ(flat_map.at(key), value);
    }
  }
}

TEST_F(FlatMapTest, updates) {
  IntFlatMap m1;

  m1.update([](auto* x) { *x = 10; }, 10);
  m1.update([](auto* x) { *x = 5; }, 5);
  m1.update([](auto* x) { *x = 15; }, 15);
  EXPECT_EQ(3, m1.size());
  EXPECT_EQ(5, m1.at(5));
  EXPECT_EQ(10, m1.at(10));
  EXPECT_EQ(15, m1.at(15));

  m1.update([](auto* x) { *x *= 2; }, 10);
  EXPECT_EQ(20, m1.at(10));

  m1.update([](auto* x) { *x -= 5; }, 5);
  EXPECT_EQ(2, m1.size());
  EXPECT_EQ(0, m1.at(5));

  m1.update([](auto* x) { *x = 0; }, 20);
  EXPECT_EQ(2, m1.size());
  EXPECT_EQ(0, m1.at(20));
}

struct StringSetPartitionInterface {
  using type = StringAbstractSet;

  static type default_value() { return type::bottom(); }

  static bool is_default_value(const type& x) { return x.is_bottom(); }

  static bool equals(const type& x, const type& y) { return x.equals(y); }

  static bool leq(const type& x, const type& y) { return x.leq(y); }

  constexpr static AbstractValueKind default_value_kind =
      AbstractValueKind::Bottom;
};

TEST_F(FlatMapTest, partitionLeq) {
  using Partition =
      FlatMap<uint32_t, StringAbstractSet, StringSetPartitionInterface>;

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
    Partition p1({{1, StringAbstractSet({"a"})}});
    Partition p2({{1, StringAbstractSet({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_TRUE(p2.leq(p1));
  }

  {
    Partition p1(
        {{2, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    Partition p2(
        {{2, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_TRUE(p2.leq(p1));
  }

  {
    Partition p1;
    Partition p2({{1, StringAbstractSet({"a"})}});
    Partition p3(
        {{2, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
    EXPECT_TRUE(p1.leq(p3));
    EXPECT_FALSE(p3.leq(p1));
  }

  {
    Partition p1({{1, StringAbstractSet({"a"})}});
    Partition p2(
        {{1, StringAbstractSet({"a"})}, {2, StringAbstractSet({"a"})}});
    Partition p3(
        {{2, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
    EXPECT_FALSE(p1.leq(p3));
    EXPECT_FALSE(p3.leq(p1));
  }

  {
    Partition p1(
        {{1, StringAbstractSet({"a"})}, {2, StringAbstractSet({"a"})}});
    Partition p2({{1, StringAbstractSet({"a"})}});
    EXPECT_FALSE(p1.leq(p2));
    EXPECT_TRUE(p2.leq(p1));
  }

  {
    Partition p1(
        {{1, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    Partition p2({{1, StringAbstractSet({"a"})},
                  {2, StringAbstractSet({"a"})},
                  {3, StringAbstractSet({"a"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
  }

  {
    Partition p1(
        {{1, StringAbstractSet({"a"})}, {3, StringAbstractSet({"b"})}});
    Partition p2({{1, StringAbstractSet({"a"})},
                  {2, StringAbstractSet({"a"})},
                  {3, StringAbstractSet({"a"})}});
    EXPECT_FALSE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
  }

  {
    Partition p1(
        {{1, StringAbstractSet({"a"})}, {3, StringAbstractSet({"b"})}});
    Partition p2({{1, StringAbstractSet({"a", "b"})},
                  {2, StringAbstractSet({"a"})},
                  {3, StringAbstractSet({"a", "b"})}});
    EXPECT_TRUE(p1.leq(p2));
    EXPECT_FALSE(p2.leq(p1));
  }
}

struct StringSetEnvironmentInterface {
  using type = StringAbstractSet;

  static type default_value() { return type::top(); }

  static bool is_default_value(const type& x) { return x.is_top(); }

  static bool equals(const type& x, const type& y) { return x.equals(y); }

  static bool leq(const type& x, const type& y) { return x.leq(y); }

  constexpr static AbstractValueKind default_value_kind =
      AbstractValueKind::Top;
};

TEST_F(FlatMapTest, environmentLeq) {
  using Environment =
      FlatMap<uint32_t, StringAbstractSet, StringSetEnvironmentInterface>;

  {
    Environment e1;
    EXPECT_TRUE(e1.leq(e1));
  }

  {
    Environment e1;
    Environment e2;
    EXPECT_TRUE(e1.leq(e2));
    EXPECT_TRUE(e2.leq(e1));
  }

  {
    Environment e1({{1, StringAbstractSet({"a"})}});
    Environment e2({{1, StringAbstractSet({"a"})}});
    EXPECT_TRUE(e1.leq(e2));
    EXPECT_TRUE(e2.leq(e1));
  }

  {
    Environment e1(
        {{2, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    Environment e2(
        {{2, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    EXPECT_TRUE(e1.leq(e2));
    EXPECT_TRUE(e2.leq(e1));
  }

  {
    Environment e1;
    Environment e2({{1, StringAbstractSet({"a"})}});
    Environment e3(
        {{2, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    EXPECT_FALSE(e1.leq(e2));
    EXPECT_TRUE(e2.leq(e1));
    EXPECT_FALSE(e1.leq(e3));
    EXPECT_TRUE(e3.leq(e1));
  }

  {
    Environment e1({{1, StringAbstractSet({"a"})}});
    Environment e2(
        {{1, StringAbstractSet({"a"})}, {2, StringAbstractSet({"a"})}});
    Environment e3(
        {{2, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    EXPECT_FALSE(e1.leq(e2));
    EXPECT_TRUE(e2.leq(e1));
    EXPECT_FALSE(e1.leq(e3));
    EXPECT_FALSE(e3.leq(e1));
  }

  {
    Environment e1(
        {{1, StringAbstractSet({"a"})}, {2, StringAbstractSet({"a"})}});
    Environment e2({{1, StringAbstractSet({"a"})}});
    EXPECT_TRUE(e1.leq(e2));
    EXPECT_FALSE(e2.leq(e1));
  }

  {
    Environment e1(
        {{1, StringAbstractSet({"a"})}, {3, StringAbstractSet({"a"})}});
    Environment e2({{1, StringAbstractSet({"a"})},
                    {2, StringAbstractSet({"a"})},
                    {3, StringAbstractSet({"a"})}});
    EXPECT_FALSE(e1.leq(e2));
    EXPECT_TRUE(e2.leq(e1));
  }

  {
    Environment e1(
        {{1, StringAbstractSet({"a"})}, {3, StringAbstractSet({"b"})}});
    Environment e2({{1, StringAbstractSet({"a"})},
                    {2, StringAbstractSet({"a"})},
                    {3, StringAbstractSet({"a"})}});
    EXPECT_FALSE(e1.leq(e2));
    EXPECT_FALSE(e2.leq(e1));
  }

  {
    Environment e1({{1, StringAbstractSet({"a", "b", "c"})},
                    {3, StringAbstractSet({"b"})}});
    Environment e2({{1, StringAbstractSet({"a", "b"})},
                    {2, StringAbstractSet({"a"})},
                    {3, StringAbstractSet({"b"})}});
    EXPECT_FALSE(e1.leq(e2));
    EXPECT_TRUE(e2.leq(e1));
  }

  {
    Environment e1({{1, StringAbstractSet({"a", "b"})},
                    {2, StringAbstractSet("c")},
                    {3, StringAbstractSet({"d", "e", "f"})},
                    {4, StringAbstractSet({"a", "f"})}});
    Environment e2({{0, StringAbstractSet({"c", "f"})},
                    {2, StringAbstractSet({"c", "d"})},
                    {3, StringAbstractSet({"d", "e", "g", "h"})}});

    EXPECT_EQ(4, e1.size());
    EXPECT_EQ(3, e2.size());

    EXPECT_FALSE(e1.leq(e2));
    EXPECT_FALSE(e2.leq(e1));
  }
}

TEST_F(FlatMapTest, unionWith) {
  auto add = [](uint32_t* a, uint32_t b) { *a += b; };

  {
    IntFlatMap p1;
    IntFlatMap p2;
    IntFlatMap p3;
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}});
    IntFlatMap p2({{1, 10}});
    IntFlatMap p3({{1, 20}});
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{2, 10}, {3, 20}});
    IntFlatMap p2({{2, 11}, {3, 21}});
    IntFlatMap p3({{2, 21}, {3, 41}});
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1;
    IntFlatMap p2({{1, 10}, {2, 20}, {3, 30}});
    IntFlatMap p3({{1, 10}, {2, 20}, {3, 30}});
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}, {2, 20}, {3, 30}});
    IntFlatMap p2;
    IntFlatMap p3({{1, 10}, {2, 20}, {3, 30}});
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}});
    IntFlatMap p2({{2, 20}, {3, 30}});
    IntFlatMap p3({{1, 10}, {2, 20}, {3, 30}});
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}});
    IntFlatMap p2({{1, 20}, {2, 40}});
    IntFlatMap p3({{1, 30}, {2, 40}});
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}, {2, 20}});
    IntFlatMap p2({{1, 1}});
    IntFlatMap p3({{1, 11}, {2, 20}});
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 1}, {3, 30}});
    IntFlatMap p2({{1, 10}, {2, 20}, {3, 30}});
    IntFlatMap p3({{1, 11}, {2, 20}, {3, 60}});
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  // Default values are removed.
  {
    IntFlatMap p1({{1, 1}, {3, 30}});
    IntFlatMap p2({{1, 10}, {2, 20}, {3, -30}});
    IntFlatMap p3({
        {1, 11},
        {2, 20},
    });
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  // Default values are removed.
  {
    IntFlatMap p1({{1, 1}, {3, 30}});
    IntFlatMap p2({{1, -1}, {3, -30}});
    IntFlatMap p3;
    p1.union_with(add, p2);
    EXPECT_EQ(p1, p3);
  }
}

TEST_F(FlatMapTest, intersectionWith) {
  auto add = [](uint32_t* a, uint32_t b) { *a += b; };

  {
    IntFlatMap p1;
    IntFlatMap p2;
    IntFlatMap p3;
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}});
    IntFlatMap p2({{1, 10}});
    IntFlatMap p3({{1, 20}});
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{2, 10}, {3, 20}});
    IntFlatMap p2({{2, 11}, {3, 21}});
    IntFlatMap p3({{2, 21}, {3, 41}});
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1;
    IntFlatMap p2({{1, 10}, {2, 20}, {3, 30}});
    IntFlatMap p3;
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}, {2, 20}, {3, 30}});
    IntFlatMap p2;
    IntFlatMap p3;
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}});
    IntFlatMap p2({{2, 20}, {3, 30}});
    IntFlatMap p3;
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}});
    IntFlatMap p2({{1, 20}, {2, 40}});
    IntFlatMap p3({{1, 30}});
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 10}, {2, 20}});
    IntFlatMap p2({{1, 1}});
    IntFlatMap p3({{1, 11}});
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  {
    IntFlatMap p1({{1, 1}, {3, 30}});
    IntFlatMap p2({{1, 10}, {2, 20}, {3, 30}});
    IntFlatMap p3({{1, 11}, {3, 60}});
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  // Default values are removed.
  {
    IntFlatMap p1({{1, 1}, {3, 30}});
    IntFlatMap p2({{1, 10}, {2, 20}, {3, -30}});
    IntFlatMap p3({{1, 11}});
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }

  // Default values are removed.
  {
    IntFlatMap p1({{1, 1}, {3, 30}, {4, 40}});
    IntFlatMap p2({{1, -1}, {3, -30}, {5, 50}});
    IntFlatMap p3;
    p1.intersection_with(add, p2);
    EXPECT_EQ(p1, p3);
  }
}

TEST_F(FlatMapTest, difference) {
  auto difference = [](IntFlatMap x, const IntFlatMap& y) -> IntFlatMap {
    auto substract = [](uint32_t* x, uint32_t y) -> void {
      // bottom - anything = bottom
      if (*x != 0) {
        *x -= y;
      }
    };
    x.difference_with(substract, y);
    return x;
  };

  EXPECT_EQ(difference(IntFlatMap(), IntFlatMap()), IntFlatMap());
  EXPECT_EQ(difference(IntFlatMap({{1, 1}}), IntFlatMap()),
            IntFlatMap({{1, 1}}));
  EXPECT_EQ(difference(IntFlatMap(), IntFlatMap({{1, 1}})), IntFlatMap());

  EXPECT_EQ(difference(IntFlatMap({{1, 1}}), IntFlatMap({{1, 1}})),
            IntFlatMap());
  EXPECT_EQ(difference(IntFlatMap({{1, 3}}), IntFlatMap({{1, 1}})),
            IntFlatMap({{1, 2}}));
  EXPECT_EQ(difference(IntFlatMap({{1, 3}}), IntFlatMap({{2, 1}})),
            IntFlatMap({{1, 3}}));
  EXPECT_EQ(difference(IntFlatMap({{1, 3}}), IntFlatMap({{1, 1}, {2, 1}})),
            IntFlatMap({{1, 2}}));

  EXPECT_EQ(difference(IntFlatMap({{1, 3}, {2, 3}}), IntFlatMap({{1, 1}})),
            IntFlatMap({{1, 2}, {2, 3}}));
  EXPECT_EQ(
      difference(IntFlatMap({{1, 3}, {2, 3}, {3, 3}}), IntFlatMap({{2, 1}})),
      IntFlatMap({{1, 3}, {2, 2}, {3, 3}}));
  EXPECT_EQ(
      difference(IntFlatMap({{1, 3}, {2, 3}, {3, 3}}), IntFlatMap({{4, 1}})),
      IntFlatMap({{1, 3}, {2, 3}, {3, 3}}));
  EXPECT_EQ(
      difference(IntFlatMap({{1, 3}, {2, 3}, {3, 3}}), IntFlatMap({{2, 3}})),
      IntFlatMap({{1, 3}, {3, 3}}));

  EXPECT_EQ(
      difference(IntFlatMap({{1, 3}, {2, 3}}), IntFlatMap({{1, 3}, {2, 3}})),
      IntFlatMap());
  EXPECT_EQ(
      difference(IntFlatMap({{1, 3}, {2, 3}}), IntFlatMap({{1, 1}, {2, 1}})),
      IntFlatMap({{1, 2}, {2, 2}}));
  EXPECT_EQ(difference(IntFlatMap({{1, 3}, {2, 3}, {3, 3}}),
                       IntFlatMap({{1, 1}, {2, 1}, {3, 1}})),
            IntFlatMap({{1, 2}, {2, 2}, {3, 2}}));

  EXPECT_EQ(difference(IntFlatMap({{1, 3}, {2, 3}, {3, 3}}),
                       IntFlatMap({{1, 1}, {2, 1}})),
            IntFlatMap({{1, 2}, {2, 2}, {3, 3}}));
  EXPECT_EQ(difference(IntFlatMap({{1, 3}, {2, 3}, {3, 3}, {4, 3}}),
                       IntFlatMap({{1, 1}, {3, 1}})),
            IntFlatMap({{1, 2}, {2, 3}, {3, 2}, {4, 3}}));

  EXPECT_EQ(difference(IntFlatMap({{1, 3}, {3, 3}}),
                       IntFlatMap({{1, 1}, {2, 1}, {3, 1}, {4, 1}})),
            IntFlatMap({{1, 2}, {3, 2}}));
  EXPECT_EQ(difference(IntFlatMap({{1, 3}, {3, 3}}),
                       IntFlatMap({{1, 1}, {2, 1}, {3, 1}})),
            IntFlatMap({{1, 2}, {3, 2}}));

  EXPECT_EQ(
      difference(IntFlatMap({{1, 3}, {3, 3}}), IntFlatMap({{2, 1}, {4, 1}})),
      IntFlatMap({{1, 3}, {3, 3}}));
  EXPECT_EQ(difference(IntFlatMap({{1, 3}, {3, 3}, {5, 3}}),
                       IntFlatMap({{2, 1}, {4, 1}, {6, 1}})),
            IntFlatMap({{1, 3}, {3, 3}, {5, 3}}));
}

TEST_F(FlatMapTest, visit) {
  auto m = IntFlatMap({
      {1, 2},
      {2, 3},
      {4, 5},
  });
  size_t sum = 0;
  m.visit([&sum](const auto& binding) { sum += binding.second; });
  EXPECT_EQ(sum, 10);
}
