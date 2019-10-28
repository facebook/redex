/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PatriciaTreeMap.h"

#include <boost/concept/assert.hpp>
#include <boost/concept_check.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unordered_map>

using namespace sparta;

using pt_map = PatriciaTreeMap<uint32_t, uint32_t>;

BOOST_CONCEPT_ASSERT((boost::ForwardContainer<pt_map>));

TEST(PatriciaTreeMapTest, basicOperations) {
  constexpr uint32_t bigint = std::numeric_limits<uint32_t>::max();
  constexpr uint32_t default_value = 0;
  pt_map m1;
  pt_map empty_map;
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

TEST(PatriciaTreeMapTest, erase_all_matching) {
  constexpr uint32_t default_value = 0;
  pt_map m1;
  m1.insert_or_assign(0, 1);
  m1.insert_or_assign(1, 1);
  m1.insert_or_assign(2, 1);
  m1.insert_or_assign(3, 1);
  m1.insert_or_assign(4, 1);

  bool any_changes = m1.erase_all_matching(0);
  EXPECT_TRUE(!any_changes);
  EXPECT_EQ(5, m1.size());

  any_changes = m1.erase_all_matching(8);
  EXPECT_TRUE(!any_changes);
  EXPECT_EQ(5, m1.size());

  any_changes = m1.erase_all_matching(2);
  EXPECT_TRUE(any_changes);
  EXPECT_EQ(3, m1.size());
  EXPECT_EQ(m1.at(2), default_value);
  EXPECT_EQ(m1.at(3), default_value);

  any_changes = m1.erase_all_matching(4);
  EXPECT_EQ(2, m1.size());
  EXPECT_EQ(m1.at(4), default_value);

  EXPECT_EQ(m1.at(0), 1);
  EXPECT_EQ(m1.at(1), 1);
}

TEST(PatriciaTreeMapTest, map) {
  constexpr uint32_t default_value = 0;
  pt_map m1;
  m1.insert_or_assign(0, 1);
  m1.insert_or_assign(1, 2);
  m1.insert_or_assign(2, 4);

  bool any_changes = m1.map([](uint32_t value) { return value; });
  EXPECT_FALSE(any_changes);
  EXPECT_EQ(3, m1.size());
  EXPECT_EQ(m1.at(0), 1);
  EXPECT_EQ(m1.at(1), 2);
  EXPECT_EQ(m1.at(2), 4);

  any_changes = m1.map([](uint32_t value) { return value - 1; });
  EXPECT_TRUE(any_changes);
  EXPECT_EQ(2, m1.size());
  EXPECT_EQ(m1.at(0), default_value);
  EXPECT_EQ(m1.at(1), 1);
  EXPECT_EQ(m1.at(2), 3);
}

TEST(PatriciaTreeMapTest, mapOfUnsignedInt64) {
  PatriciaTreeMap<uint64_t, std::string> m;
  std::unordered_map<uint64_t, std::string> entries = {
      {0, "zero"}, {1, "one"}, {2, "two"}, {10, "ten"}, {4000000000, "many"}};

  for (auto e : entries) {
    m.insert_or_assign(e.first, e.second);
  }
  EXPECT_EQ(entries.size(), m.size());
  for (auto e : m) {
    auto it = entries.find(e.first);
    EXPECT_NE(entries.end(), it);
    EXPECT_EQ(it->second, e.second);
  }
}
