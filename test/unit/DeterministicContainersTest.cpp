/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DeterministicContainers.h"

#include <gtest/gtest.h>

class DeterministicContainersTest : public ::testing::Test {
 protected:
  DeterministicContainersTest() {}
};

TEST_F(DeterministicContainersTest, unordered_any) {
  UnorderedMap<int, int> map{{1, 42}};
  EXPECT_EQ(1, unordered_any(map)->first);
  EXPECT_EQ(42, unordered_any(map)->second);
}

TEST_F(DeterministicContainersTest, UnorderedIterable) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  std::vector<std::pair<int, int>> vec;
  for (auto& p : UnorderedIterable(map)) {
    vec.push_back(p);
  }
  std::sort(vec.begin(), vec.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  EXPECT_EQ(2, vec.size());
  EXPECT_EQ(1, vec[0].first);
  EXPECT_EQ(2, vec[1].first);
}

TEST_F(DeterministicContainersTest, unordered_order) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  auto ordered = unordered_order(
      map, [](const auto& p, const auto& q) { return p.first < q.first; });
  EXPECT_EQ(2, ordered.size());
  EXPECT_EQ(1, ordered[0].first);
  EXPECT_EQ(2, ordered[1].first);
}

TEST_F(DeterministicContainersTest, unordered_order_keys) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  auto keys = unordered_order_keys(map);
  EXPECT_EQ(2, keys.size());
  EXPECT_EQ(1, keys[0]);
  EXPECT_EQ(2, keys[1]);
}

TEST_F(DeterministicContainersTest, unordered_accumulate) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  auto sum = unordered_accumulate(
      map, 0, [](int a, const auto& p) { return a + p.second; });
  EXPECT_EQ(42 + 23, sum);
}

TEST_F(DeterministicContainersTest, unordered_all_of) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  EXPECT_TRUE(unordered_all_of(map, [](auto& p) { return p.second >= 23; }));
  EXPECT_FALSE(unordered_all_of(map, [](auto& p) { return p.second < 23; }));
}

TEST_F(DeterministicContainersTest, unordered_any_of) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  EXPECT_TRUE(unordered_any_of(map, [](auto& p) { return p.second >= 42; }));
  EXPECT_TRUE(unordered_any_of(map, [](auto& p) { return p.second < 42; }));
}

TEST_F(DeterministicContainersTest, unordered_none_of) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  EXPECT_FALSE(unordered_none_of(map, [](auto& p) { return p.second >= 23; }));
  EXPECT_TRUE(unordered_none_of(map, [](auto& p) { return p.second < 23; }));
  EXPECT_FALSE(unordered_none_of(map, [](auto& p) { return p.second >= 42; }));
  EXPECT_FALSE(unordered_none_of(map, [](auto& p) { return p.second < 42; }));
}

TEST_F(DeterministicContainersTest, unordered_for_each) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  UnorderedMap<int, int> copy;
  unordered_for_each(map, [&](auto& p) { copy.insert(p); });
  ASSERT_EQ(2, copy.size());
  EXPECT_EQ(1 + 2, unordered_accumulate(copy, 0, [](int a, const auto& p) {
              return a + p.first;
            }));
  EXPECT_EQ(42 + 23, unordered_accumulate(copy, 0, [](int a, const auto& p) {
              return a + p.second;
            }));
}

TEST_F(DeterministicContainersTest, unordered_copy) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  std::vector<std::pair<int, int>> copy(2);
  unordered_copy(map, copy.begin());
  EXPECT_EQ(2, copy.size());
  EXPECT_EQ(1 + 2, copy[0].first + copy[1].first);
}

TEST_F(DeterministicContainersTest, unordered_copy_if) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  std::vector<std::pair<int, int>> copy(1);
  unordered_copy_if(map, copy.begin(), [](auto& p) { return p.second >= 42; });
  EXPECT_EQ(1, copy.size());
  EXPECT_EQ(1, copy[0].first);
  EXPECT_EQ(42, copy[0].second);
}

TEST_F(DeterministicContainersTest, unordered_erase_if) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  unordered_erase_if(map, [](auto& p) { return p.second >= 42; });
  EXPECT_EQ(1, map.size());
  EXPECT_EQ(2, unordered_any(map)->first);
  EXPECT_EQ(23, unordered_any(map)->second);
}

TEST_F(DeterministicContainersTest, unordered_transform) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  std::vector<std::pair<int, int>> copy(2);
  unordered_transform(map, copy.begin(), [](auto& p) {
    return std::make_pair(p.first + 1, p.second + 1);
  });
  EXPECT_EQ(2, copy.size());
  EXPECT_EQ(1 + 1 + 2 + 1, copy[0].first + copy[1].first);
  EXPECT_EQ(42 + 1 + 23 + 1, copy[0].second + copy[1].second);
}

TEST_F(DeterministicContainersTest, insert_unordered_iterable) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  UnorderedMap<int, int> copy;
  insert_unordered_iterable(copy, map);
  EXPECT_EQ(2, copy.size());
  EXPECT_EQ(1 + 2, unordered_accumulate(copy, 0, [](int a, const auto& p) {
              return a + p.first;
            }));
  EXPECT_EQ(42 + 23, unordered_accumulate(copy, 0, [](int a, const auto& p) {
              return a + p.second;
            }));
}
