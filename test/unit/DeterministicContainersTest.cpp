/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DeterministicContainers.h"

#include <gtest/gtest.h>
#include <map>
#include <vector>

class DeterministicContainersTest : public ::testing::Test {
 protected:
  DeterministicContainersTest() {}
};

TEST_F(DeterministicContainersTest, unordered_any_map) {
  UnorderedMap<int, int> map{{1, 42}};
  EXPECT_EQ(1, unordered_any(map)->first);
  EXPECT_EQ(42, unordered_any(map)->second);
}

TEST_F(DeterministicContainersTest, unordered_any_set) {
  UnorderedSet<int> set({23});
  EXPECT_EQ(23, *unordered_any(set));
}

TEST_F(DeterministicContainersTest, unordered_any_set_empty) {
  UnorderedSet<int> set;
  EXPECT_EQ(set.end(), unordered_any(set));
}

TEST_F(DeterministicContainersTest, UnorderedIterable_map) {
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

TEST_F(DeterministicContainersTest, UnorderedIterable_set) {
  UnorderedSet<int> set({23, 42});
  std::vector<int> vec;
  for (int a : UnorderedIterable(set)) {
    vec.push_back(a);
  }
  EXPECT_EQ(2, vec.size());
  EXPECT_EQ(23 + 42,
            unordered_accumulate(set, 0, [](int a, int b) { return a + b; }));
}

TEST_F(DeterministicContainersTest, unordered_to_ordered_map) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  auto ordered = unordered_to_ordered(
      map, [](const auto& p, const auto& q) { return p.first < q.first; });
  EXPECT_EQ(2, ordered.size());
  EXPECT_EQ(1, ordered[0].first);
  EXPECT_EQ(2, ordered[1].first);
}

TEST_F(DeterministicContainersTest, unordered_to_ordered_set) {
  UnorderedSet<int> set{1, 11, 7, 3, 5};
  auto ordered = unordered_to_ordered(set, [](int a, int b) { return a < b; });
  EXPECT_EQ(std::vector<int>({1, 3, 5, 7, 11}), ordered);
}

TEST_F(DeterministicContainersTest, unordered_to_ordered_keys) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  auto keys = unordered_to_ordered_keys(map);
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

TEST_F(DeterministicContainersTest, insert_unordered_iterable_vector) {
  UnorderedSet<int> set{1, 11, 7, 3, 5};
  std::vector<int> copy;
  insert_unordered_iterable(copy, copy.end(), set);
  EXPECT_EQ(5, copy.size());
  EXPECT_EQ(1 + 11 + 7 + 3 + 5,
            unordered_accumulate(set, 0, [](int a, int b) { return a + b; }));
}

TEST_F(DeterministicContainersTest, unordered_min_element_set) {
  UnorderedSet<int> set{42, 23, 7, 11, 5};
  auto min = unordered_min_element(set);
  EXPECT_EQ(5, *min);
}

TEST_F(DeterministicContainersTest, unordered_min_element_map) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}, {3, 7}};
  auto min = unordered_min_element(
      map, [](const auto& a, const auto& b) { return a.second < b.second; });
  EXPECT_EQ(3, min->first);
  EXPECT_EQ(7, min->second);
}

TEST_F(DeterministicContainersTest, unordered_min_element_set_custom) {
  UnorderedSet<int> set{-5, 4, -3, 2, -1};
  auto min = unordered_min_element(
      set, [](int a, int b) { return std::abs(a) < std::abs(b); });
  EXPECT_EQ(-1, *min);
}

TEST_F(DeterministicContainersTest, unordered_min_element_map_custom) {
  UnorderedMap<std::string, int> map{{"abc", 1}, {"a", 2}, {"abcd", 3}};
  auto min = unordered_min_element(map, [](const auto& a, const auto& b) {
    return a.first.length() < b.first.length();
  });
  EXPECT_EQ("a", min->first);
  EXPECT_EQ(2, min->second);
}

TEST_F(DeterministicContainersTest, unordered_max_element_set) {
  UnorderedSet<int> set{42, 23, 7, 11, 5};
  auto max = unordered_max_element(set);
  EXPECT_EQ(42, *max);
}

TEST_F(DeterministicContainersTest, unordered_max_element_map) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}, {3, 7}};
  auto max = unordered_max_element(
      map, [](const auto& a, const auto& b) { return a.second < b.second; });
  EXPECT_EQ(1, max->first);
  EXPECT_EQ(42, max->second);
}

TEST_F(DeterministicContainersTest, unordered_max_element_set_custom) {
  UnorderedSet<int> set{-5, 4, -3, 2, -1};
  auto max = unordered_max_element(
      set, [](int a, int b) { return std::abs(a) < std::abs(b); });
  EXPECT_EQ(-5, *max);
}

TEST_F(DeterministicContainersTest, unordered_min_element_stdmap) {
  std::map<int, int> map{{1, 42}, {2, 23}, {3, 7}};
  auto min = unordered_min_element(
      map, [](const auto& a, const auto& b) { return a.second < b.second; });
  EXPECT_EQ(3, min->first);
  EXPECT_EQ(7, min->second);
}

TEST_F(DeterministicContainersTest, unordered_min_element_stdmap_custom) {
  std::map<std::string, int> map{{"abc", 1}, {"a", 2}, {"abcd", 3}};
  auto min = unordered_min_element(map, [](const auto& a, const auto& b) {
    return a.first.length() < b.first.length();
  });
  EXPECT_EQ("a", min->first);
  EXPECT_EQ(2, min->second);
}

TEST_F(DeterministicContainersTest, unordered_max_element_stdmap) {
  std::map<int, int> map{{1, 42}, {2, 23}, {3, 7}};
  auto max = unordered_max_element(
      map, [](const auto& a, const auto& b) { return a.second < b.second; });
  EXPECT_EQ(1, max->first);
  EXPECT_EQ(42, max->second);
}

TEST_F(DeterministicContainersTest, unordered_max_element_stdmap_custom) {
  std::map<std::string, int> map{{"abc", 1}, {"a", 2}, {"abcd", 3}};
  auto max = unordered_max_element(map, [](const auto& a, const auto& b) {
    return a.first.length() < b.first.length();
  });
  EXPECT_EQ("abcd", max->first);
  EXPECT_EQ(3, max->second);
}

TEST_F(DeterministicContainersTest, unordered_min_element_vector) {
  std::vector<int> vec{42, 23, 7, 11, 5};
  auto min = unordered_min_element(vec);
  EXPECT_EQ(5, *min);
}

TEST_F(DeterministicContainersTest, unordered_min_element_vector_custom) {
  std::vector<int> vec{-5, 4, -3, 2, -1};
  auto min = unordered_min_element(
      vec, [](int a, int b) { return std::abs(a) < std::abs(b); });
  EXPECT_EQ(-1, *min);
}

TEST_F(DeterministicContainersTest, unordered_max_element_vector) {
  std::vector<int> vec{42, 23, 7, 11, 5};
  auto max = unordered_max_element(vec);
  EXPECT_EQ(42, *max);
}

TEST_F(DeterministicContainersTest, unordered_max_element_vector_custom) {
  std::vector<int> vec{-5, 4, -3, 2, -1};
  auto max = unordered_max_element(
      vec, [](int a, int b) { return std::abs(a) < std::abs(b); });
  EXPECT_EQ(-5, *max);
}
