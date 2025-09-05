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
    vec.emplace_back(p);
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

TEST_F(DeterministicContainersTest, UnorderedBag_construction) {
  UnorderedBag<int> bag{1, 2, 3};
  EXPECT_EQ(3, bag.size());
  EXPECT_FALSE(bag.empty());

  UnorderedBag<int> empty_bag;
  EXPECT_EQ(0, empty_bag.size());
  EXPECT_TRUE(empty_bag.empty());
}

TEST_F(DeterministicContainersTest, UnorderedBag_basic_operations) {
  UnorderedBag<int> bag;
  EXPECT_TRUE(bag.empty());

  bag.emplace(42);
  EXPECT_EQ(1, bag.size());
  EXPECT_FALSE(bag.empty());

  bag.emplace(23);
  EXPECT_EQ(2, bag.size());

  bag.emplace(15);
  bag.emplace(7);
  EXPECT_EQ(4, bag.size());
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_any) {
  UnorderedBag<int> bag;
  bag.emplace(42);
  bag.emplace(23);
  auto any = *unordered_any(bag);
  EXPECT_TRUE(any == 42 || any == 23);

  UnorderedBag<int> empty_bag;
  EXPECT_EQ(empty_bag.end(), unordered_any(empty_bag));
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_accumulate) {
  UnorderedBag<int> bag{1, 2, 3, 4, 5};
  int sum =
      unordered_accumulate(bag, 0, [](int acc, int val) { return acc + val; });
  EXPECT_EQ(15, sum);
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_transform) {
  UnorderedBag<int> bag{1, 2, 3};
  std::vector<int> result(3);
  unordered_transform(bag, result.begin(), [](int val) { return val * 2; });
  EXPECT_EQ(12, unordered_accumulate(
                    result, 0, [](int acc, int val) { return acc + val; }));
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_copy) {
  UnorderedBag<int> bag{1, 2, 3, 4};
  std::vector<int> copy(4);
  unordered_copy(bag, copy.begin());
  EXPECT_EQ(4, copy.size());
  EXPECT_EQ(10, unordered_accumulate(
                    copy, 0, [](int acc, int val) { return acc + val; }));
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_min_element) {
  UnorderedBag<int> bag{42, 23, 7, 11, 5};
  auto min = unordered_min_element(bag);
  EXPECT_EQ(5, *min);
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_max_element) {
  UnorderedBag<int> bag{42, 23, 7, 11, 5};
  auto max = unordered_max_element(bag);
  EXPECT_EQ(42, *max);
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_erase_if) {
  UnorderedBag<int> bag{42, 23, 7, 11, 5};
  unordered_erase_if(bag, [](int x) { return x > 20; });
  EXPECT_EQ(3, bag.size());
  EXPECT_EQ(23, unordered_accumulate(
                    bag, 0, [](int acc, int val) { return acc + val; }));
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_min_element_custom) {
  UnorderedBag<int> bag{-5, 4, -3, 2, -1};
  auto min = unordered_min_element(
      bag, [](int a, int b) { return std::abs(a) < std::abs(b); });
  EXPECT_EQ(-1, *min);
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_max_element_custom) {
  UnorderedBag<int> bag{-5, 4, -3, 2, -1};
  auto max = unordered_max_element(
      bag, [](int a, int b) { return std::abs(a) < std::abs(b); });
  EXPECT_EQ(-5, *max);
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_to_ordered) {
  UnorderedBag<int> bag{5, 2, 8, 1, 9, 3};
  auto ordered = unordered_to_ordered(bag, [](int a, int b) { return a < b; });
  EXPECT_EQ(std::vector<int>({1, 2, 3, 5, 8, 9}), ordered);
}

TEST_F(DeterministicContainersTest,
       UnorderedBag_unordered_erase_if_divisible_by_3) {
  UnorderedBag<int> bag{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  unordered_erase_if(bag, [](int x) { return x % 3 == 0; });
  EXPECT_EQ(7, bag.size());
  auto ordered = unordered_to_ordered(bag, [](int a, int b) { return a < b; });
  EXPECT_EQ(std::vector<int>({1, 2, 4, 5, 7, 8, 10}), ordered);
}

TEST_F(DeterministicContainersTest, unordered_find_map) {
  UnorderedMap<int, std::string> map{{1, "one"}, {2, "two"}, {3, "three"}};

  // Test finding an existing pair
  auto found = unordered_find(map, std::pair<const int, std::string>(2, "two"));
  EXPECT_NE(map.end(), found);
  EXPECT_EQ(2, found->first);
  EXPECT_EQ("two", found->second);

  // Test finding a non-existent pair (existing key, wrong value)
  auto not_found1 =
      unordered_find(map, std::pair<const int, std::string>(2, "three"));
  EXPECT_EQ(map.end(), not_found1);

  // Test finding a non-existent pair (non-existent key)
  auto not_found2 =
      unordered_find(map, std::pair<const int, std::string>(4, "four"));
  EXPECT_EQ(map.end(), not_found2);
}

TEST_F(DeterministicContainersTest, unordered_find_set) {
  UnorderedSet<int> set{1, 2, 3, 4, 5};
  auto found = unordered_find(set, 3);
  EXPECT_NE(set.end(), found);
  EXPECT_EQ(3, *found);

  auto not_found = unordered_find(set, 6);
  EXPECT_EQ(set.end(), not_found);
}

TEST_F(DeterministicContainersTest, unordered_find_if_map) {
  UnorderedMap<int, int> map{{1, 10}, {2, 20}, {3, 30}};
  auto found =
      unordered_find_if(map, [](const auto& p) { return p.second > 25; });
  EXPECT_NE(map.end(), found);
  EXPECT_EQ(30, found->second);

  auto not_found =
      unordered_find_if(map, [](const auto& p) { return p.second > 50; });
  EXPECT_EQ(map.end(), not_found);
}

TEST_F(DeterministicContainersTest, unordered_find_if_set) {
  UnorderedSet<int> set{1, 2, 3, 4, 5};
  auto found = unordered_find_if(set, [](int x) { return x > 3; });
  EXPECT_NE(set.end(), found);
  EXPECT_TRUE(*found == 4 || *found == 5);

  auto not_found = unordered_find_if(set, [](int x) { return x > 10; });
  EXPECT_EQ(set.end(), not_found);
}

TEST_F(DeterministicContainersTest, unordered_find_if_not_map) {
  UnorderedMap<int, int> map{{1, 10}, {2, 20}, {3, 30}};
  auto found =
      unordered_find_if_not(map, [](const auto& p) { return p.second > 25; });
  EXPECT_NE(map.end(), found);
  EXPECT_TRUE(found->second <= 25);

  auto not_found =
      unordered_find_if_not(map, [](const auto& p) { return p.second > 0; });
  EXPECT_EQ(map.end(), not_found);
}

TEST_F(DeterministicContainersTest, unordered_find_if_not_set) {
  UnorderedSet<int> set{1, 2, 3, 4, 5};
  auto found = unordered_find_if_not(set, [](int x) { return x > 3; });
  EXPECT_NE(set.end(), found);
  EXPECT_TRUE(*found <= 3);

  auto not_found = unordered_find_if_not(set, [](int x) { return x > 0; });
  EXPECT_EQ(set.end(), not_found);
}
