/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/PatriciaTreeMap.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <initializer_list>
#include <unordered_map>

#include <boost/concept/assert.hpp>
#include <boost/concept_check.hpp>

using namespace sparta;

using pt_map = PatriciaTreeMap<uint32_t, uint32_t>;

BOOST_CONCEPT_ASSERT((boost::ForwardContainer<pt_map>));

namespace {

pt_map create_pt_map(
    std::initializer_list<std::pair<uint32_t, uint32_t>> pairs) {
  pt_map map;
  for (const auto& p : pairs) {
    map.insert_or_assign(p.first, p.second);
  }
  return map;
}

} // namespace

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
  pt_map m1 = create_pt_map({{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}});

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

TEST(PatriciaTreeMapTest, transform) {
  constexpr uint32_t default_value = 0;
  pt_map m1 = create_pt_map({{0, 1}, {1, 2}, {2, 4}});

  bool any_changes = m1.transform([](uint32_t value) { return value; });
  EXPECT_FALSE(any_changes);
  EXPECT_EQ(3, m1.size());
  EXPECT_EQ(m1.at(0), 1);
  EXPECT_EQ(m1.at(1), 2);
  EXPECT_EQ(m1.at(2), 4);

  any_changes = m1.transform([](uint32_t value) { return value - 1; });
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

TEST(PatriciaTreeMapTest, difference) {
  auto substract = [](uint32_t x, uint32_t y) -> uint32_t {
    if (x == 0) {
      // bottom - anything = bottom
      return 0;
    } else {
      return x - y;
    }
  };

  EXPECT_EQ(pt_map().get_difference_with(substract, pt_map()), pt_map());
  EXPECT_EQ(create_pt_map({{1, 1}}).get_difference_with(substract, pt_map()),
            create_pt_map({{1, 1}}));
  EXPECT_EQ(pt_map().get_difference_with(substract, create_pt_map({{1, 1}})),
            pt_map());

  // lhs is a leaf.
  EXPECT_EQ(create_pt_map({{1, 1}}).get_difference_with(
                substract, create_pt_map({{1, 1}})),
            pt_map());
  EXPECT_EQ(create_pt_map({{1, 3}}).get_difference_with(
                substract, create_pt_map({{1, 1}})),
            create_pt_map({{1, 2}}));
  EXPECT_EQ(create_pt_map({{1, 3}}).get_difference_with(
                substract, create_pt_map({{2, 1}})),
            create_pt_map({{1, 3}}));
  EXPECT_EQ(create_pt_map({{1, 3}}).get_difference_with(
                substract, create_pt_map({{1, 1}, {2, 1}})),
            create_pt_map({{1, 2}}));

  // rhs is a leaf.
  EXPECT_EQ(create_pt_map({{1, 3}, {2, 3}})
                .get_difference_with(substract, create_pt_map({{1, 1}})),
            create_pt_map({{1, 2}, {2, 3}}));
  EXPECT_EQ(create_pt_map({{1, 3}, {2, 3}, {3, 3}})
                .get_difference_with(substract, create_pt_map({{2, 1}})),
            create_pt_map({{1, 3}, {2, 2}, {3, 3}}));
  EXPECT_EQ(create_pt_map({{1, 3}, {2, 3}, {3, 3}})
                .get_difference_with(substract, create_pt_map({{4, 1}})),
            create_pt_map({{1, 3}, {2, 3}, {3, 3}}));
  EXPECT_EQ(create_pt_map({{1, 3}, {2, 3}, {3, 3}})
                .get_difference_with(substract, create_pt_map({{2, 3}})),
            create_pt_map({{1, 3}, {3, 3}}));

  // lhs and rhs have common prefixes.
  EXPECT_EQ(
      create_pt_map({{1, 3}, {2, 3}})
          .get_difference_with(substract, create_pt_map({{1, 3}, {2, 3}})),
      pt_map());
  EXPECT_EQ(
      create_pt_map({{1, 3}, {2, 3}})
          .get_difference_with(substract, create_pt_map({{1, 1}, {2, 1}})),
      create_pt_map({{1, 2}, {2, 2}}));
  EXPECT_EQ(create_pt_map({{1, 3}, {2, 3}, {3, 3}})
                .get_difference_with(substract,
                                     create_pt_map({{1, 1}, {2, 1}, {3, 1}})),
            create_pt_map({{1, 2}, {2, 2}, {3, 2}}));

  // rhs is included in lhs.
  EXPECT_EQ(
      create_pt_map({{1, 3}, {2, 3}, {3, 3}})
          .get_difference_with(substract, create_pt_map({{1, 1}, {2, 1}})),
      create_pt_map({{1, 2}, {2, 2}, {3, 3}}));
  EXPECT_EQ(
      create_pt_map({{1, 3}, {2, 3}, {3, 3}, {4, 3}})
          .get_difference_with(substract, create_pt_map({{1, 1}, {3, 1}})),
      create_pt_map({{1, 2}, {2, 3}, {3, 2}, {4, 3}}));

  // lhs is included in rhs.
  EXPECT_EQ(create_pt_map({{1, 3}, {3, 3}})
                .get_difference_with(
                    substract, create_pt_map({{1, 1}, {2, 1}, {3, 1}, {4, 1}})),
            create_pt_map({{1, 2}, {3, 2}}));
  EXPECT_EQ(create_pt_map({{1, 3}, {3, 3}})
                .get_difference_with(substract,
                                     create_pt_map({{1, 1}, {2, 1}, {3, 1}})),
            create_pt_map({{1, 2}, {3, 2}}));

  // lhs and rhs have different prefixes.
  EXPECT_EQ(
      create_pt_map({{1, 3}, {3, 3}})
          .get_difference_with(substract, create_pt_map({{2, 1}, {4, 1}})),
      create_pt_map({{1, 3}, {3, 3}}));
  EXPECT_EQ(create_pt_map({{1, 3}, {3, 3}, {5, 3}})
                .get_difference_with(substract,
                                     create_pt_map({{2, 1}, {4, 1}, {6, 1}})),
            create_pt_map({{1, 3}, {3, 3}, {5, 3}}));
}

TEST(PatriciaTreeMapTest, movableOperators) {
  pt_map p = create_pt_map({{0, 1}, {1, 2}});

  // lambda passed by rvalue reference, holding a non-copyable value.
  auto movable = std::make_unique<uint32_t>(3);
  p.update(
      [movable = std::move(movable)](uint32_t value) mutable {
        auto tmp = std::move(movable);
        return value + *tmp;
      },
      0);
  EXPECT_EQ(p.at(0), 4);

  // lambda passed by lvalue reference, holding a non-copyable value.
  movable = std::make_unique<uint32_t>(4);
  auto updater = [movable = std::move(movable)](uint32_t value) mutable {
    auto tmp = std::move(movable);
    return value + *tmp;
  };
  p.update(updater, 0);
  EXPECT_EQ(p.at(0), 8);

  // lambda passed by rvalue reference, holding a non-copyable value.
  movable = std::make_unique<uint32_t>(10);
  p.transform([movable = std::move(movable)](uint32_t /* value */) mutable {
    auto tmp = std::move(movable);
    (*tmp)++;
    auto new_value = *tmp;
    movable = std::move(tmp);
    return new_value;
  });
  EXPECT_EQ(p.at(0), 11);
  EXPECT_EQ(p.at(1), 12);

  // lambda passed by lvalue reference, holding a non-copyable value.
  movable = std::make_unique<uint32_t>(20);
  auto transformer = [movable =
                          std::move(movable)](uint32_t /* value */) mutable {
    auto tmp = std::move(movable);
    (*tmp)++;
    auto new_value = *tmp;
    movable = std::move(tmp);
    return new_value;
  };
  p.transform(transformer);
  EXPECT_EQ(p.at(0), 21);
  EXPECT_EQ(p.at(1), 22);
}
