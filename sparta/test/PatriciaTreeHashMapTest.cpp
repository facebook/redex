/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/PatriciaTreeHashMap.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <initializer_list>
#include <unordered_map>

#include <boost/concept/assert.hpp>
#include <boost/concept_check.hpp>

using namespace sparta;

using pth_map = PatriciaTreeHashMap<uint32_t, uint32_t>;

BOOST_CONCEPT_ASSERT((boost::ForwardContainer<pth_map>));

namespace {

pth_map create_pth_map(
    std::initializer_list<std::pair<uint32_t, uint32_t>> pairs) {
  pth_map map;
  for (const auto& p : pairs) {
    map.insert_or_assign(p.first, p.second);
  }
  return map;
}

} // namespace

TEST(PatriciaTreeHashMapTest, basicOperations) {
  constexpr uint32_t bigint = std::numeric_limits<uint32_t>::max();
  constexpr uint32_t default_value = 0;
  pth_map m1;
  pth_map empty_map;
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

TEST(PatriciaTreeHashMapTest, map) {
  constexpr uint32_t default_value = 0;
  pth_map m1 = create_pth_map({{0, 1}, {1, 2}, {2, 4}});

  bool any_changes = m1.transform([](uint32_t*) {});
  EXPECT_FALSE(any_changes);
  EXPECT_EQ(3, m1.size());
  EXPECT_EQ(m1.at(0), 1);
  EXPECT_EQ(m1.at(1), 2);
  EXPECT_EQ(m1.at(2), 4);

  any_changes = m1.transform([](uint32_t* value) { --(*value); });
  EXPECT_TRUE(any_changes);
  EXPECT_EQ(2, m1.size());
  EXPECT_EQ(m1.at(0), default_value);
  EXPECT_EQ(m1.at(1), 1);
  EXPECT_EQ(m1.at(2), 3);
}

TEST(PatriciaTreeHashMapTest, mapOfUnsignedInt64) {
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

TEST(PatriciaTreeHashMapTest, difference) {
  auto substract = [](uint32_t* x, uint32_t y) -> void {
    // bottom - anything = bottom
    if (*x != 0) {
      *x -= y;
    }
  };

  EXPECT_EQ(pth_map().get_difference_with(substract, pth_map()), pth_map());
  EXPECT_EQ(create_pth_map({{1, 1}}).get_difference_with(substract, pth_map()),
            create_pth_map({{1, 1}}));
  EXPECT_EQ(pth_map().get_difference_with(substract, create_pth_map({{1, 1}})),
            pth_map());

  // lhs is a leaf.
  EXPECT_EQ(create_pth_map({{1, 1}}).get_difference_with(
                substract, create_pth_map({{1, 1}})),
            pth_map());
  EXPECT_EQ(create_pth_map({{1, 3}}).get_difference_with(
                substract, create_pth_map({{1, 1}})),
            create_pth_map({{1, 2}}));
  EXPECT_EQ(create_pth_map({{1, 3}}).get_difference_with(
                substract, create_pth_map({{2, 1}})),
            create_pth_map({{1, 3}}));
  EXPECT_EQ(create_pth_map({{1, 3}}).get_difference_with(
                substract, create_pth_map({{1, 1}, {2, 1}})),
            create_pth_map({{1, 2}}));

  // rhs is a leaf.
  EXPECT_EQ(create_pth_map({{1, 3}, {2, 3}})
                .get_difference_with(substract, create_pth_map({{1, 1}})),
            create_pth_map({{1, 2}, {2, 3}}));
  EXPECT_EQ(create_pth_map({{1, 3}, {2, 3}, {3, 3}})
                .get_difference_with(substract, create_pth_map({{2, 1}})),
            create_pth_map({{1, 3}, {2, 2}, {3, 3}}));
  EXPECT_EQ(create_pth_map({{1, 3}, {2, 3}, {3, 3}})
                .get_difference_with(substract, create_pth_map({{4, 1}})),
            create_pth_map({{1, 3}, {2, 3}, {3, 3}}));
  EXPECT_EQ(create_pth_map({{1, 3}, {2, 3}, {3, 3}})
                .get_difference_with(substract, create_pth_map({{2, 3}})),
            create_pth_map({{1, 3}, {3, 3}}));

  // lhs and rhs have common prefixes.
  EXPECT_EQ(
      create_pth_map({{1, 3}, {2, 3}})
          .get_difference_with(substract, create_pth_map({{1, 3}, {2, 3}})),
      pth_map());
  EXPECT_EQ(
      create_pth_map({{1, 3}, {2, 3}})
          .get_difference_with(substract, create_pth_map({{1, 1}, {2, 1}})),
      create_pth_map({{1, 2}, {2, 2}}));
  EXPECT_EQ(create_pth_map({{1, 3}, {2, 3}, {3, 3}})
                .get_difference_with(substract,
                                     create_pth_map({{1, 1}, {2, 1}, {3, 1}})),
            create_pth_map({{1, 2}, {2, 2}, {3, 2}}));

  // rhs is included in lhs.
  EXPECT_EQ(
      create_pth_map({{1, 3}, {2, 3}, {3, 3}})
          .get_difference_with(substract, create_pth_map({{1, 1}, {2, 1}})),
      create_pth_map({{1, 2}, {2, 2}, {3, 3}}));
  EXPECT_EQ(
      create_pth_map({{1, 3}, {2, 3}, {3, 3}, {4, 3}})
          .get_difference_with(substract, create_pth_map({{1, 1}, {3, 1}})),
      create_pth_map({{1, 2}, {2, 3}, {3, 2}, {4, 3}}));

  // lhs is included in rhs.
  EXPECT_EQ(
      create_pth_map({{1, 3}, {3, 3}})
          .get_difference_with(
              substract, create_pth_map({{1, 1}, {2, 1}, {3, 1}, {4, 1}})),
      create_pth_map({{1, 2}, {3, 2}}));
  EXPECT_EQ(create_pth_map({{1, 3}, {3, 3}})
                .get_difference_with(substract,
                                     create_pth_map({{1, 1}, {2, 1}, {3, 1}})),
            create_pth_map({{1, 2}, {3, 2}}));

  // lhs and rhs have different prefixes.
  EXPECT_EQ(
      create_pth_map({{1, 3}, {3, 3}})
          .get_difference_with(substract, create_pth_map({{2, 1}, {4, 1}})),
      create_pth_map({{1, 3}, {3, 3}}));
  EXPECT_EQ(create_pth_map({{1, 3}, {3, 3}, {5, 3}})
                .get_difference_with(substract,
                                     create_pth_map({{2, 1}, {4, 1}, {6, 1}})),
            create_pth_map({{1, 3}, {3, 3}, {5, 3}}));
}
