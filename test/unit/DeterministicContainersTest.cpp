/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DeterministicContainers.h"
#include "RedexTest.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <vector>

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

class DeterministicContainersTest : public RedexTest {
 protected:
  DeterministicContainersTest() {}
};

TEST_F(DeterministicContainersTest, unordered_any_map) {
  UnorderedMap<int, int> map{{1, 42}};
  EXPECT_EQ(1, unordered_any(map)->first);
  EXPECT_EQ(42, unordered_any(map)->second);
}

TEST_F(DeterministicContainersTest, unordered_any_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}};
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
  constexpr std::array<std::pair<int, int>, 2u> map_values{{{1, 42}, {2, 23}}};
  UnorderedMap<int, int> map{map_values[0], map_values[1]};
  std::vector<std::pair<int, int>> vec;
  for (auto& p : UnorderedIterable(map)) {
    vec.push_back(p);
  }
  EXPECT_THAT(vec, UnorderedElementsAreArray(map_values));
}

TEST_F(DeterministicContainersTest, UnorderedIterable_multimap) {
  constexpr std::array<std::pair<int, int>, 3u> map_values{
      {{1, 42}, {1, 45}, {2, 23}}};
  UnorderedMultiMap<int, int> map{map_values[0], map_values[1], map_values[2]};
  std::vector<std::pair<int, int>> vec;
  for (auto& p : UnorderedIterable(map)) {
    vec.push_back(p);
  }
  EXPECT_THAT(vec, UnorderedElementsAreArray(map_values));
}

TEST_F(DeterministicContainersTest, UnorderedIterable_set) {
  constexpr std::array<int, 2u> set_values{23, 42};
  UnorderedSet<int> set({set_values[0], set_values[1]});
  std::vector<int> vec;
  for (int a : UnorderedIterable(set)) {
    vec.push_back(a);
  }
  EXPECT_THAT(vec, UnorderedElementsAreArray(set_values));
}

TEST_F(DeterministicContainersTest, unordered_to_ordered_map) {
  constexpr std::array<std::pair<int, int>, 2u> map_values{{{1, 42}, {2, 23}}};
  UnorderedMap<int, int> map{map_values[0], map_values[1]};
  auto ordered = unordered_to_ordered(
      map, [](const auto& p, const auto& q) { return p.first < q.first; });
  EXPECT_THAT(ordered, ElementsAreArray(map_values));
}

TEST_F(DeterministicContainersTest, unordered_to_ordered_multimap) {
  constexpr std::array<std::pair<int, int>, 3u> map_values{
      {{1, 42}, {1, 45}, {2, 23}}};
  UnorderedMultiMap<int, int> map{map_values[0], map_values[1], map_values[2]};
  auto ordered = unordered_to_ordered(map, [](const auto& a, const auto& b) {
    return a.first == b.first ? a.second < b.second : a.first < b.first;
  });
  EXPECT_THAT(ordered, ElementsAreArray(map_values));
}

TEST_F(DeterministicContainersTest, unordered_to_ordered_set) {
  constexpr std::array<int, 5u> set_values{1, 3, 5, 7, 11};
  UnorderedSet<int> set{set_values[0], set_values[1], set_values[2],
                        set_values[3], set_values[4]};
  auto ordered = unordered_to_ordered(set, [](int a, int b) { return a < b; });
  EXPECT_THAT(ordered, ElementsAreArray(set_values));
}

TEST_F(DeterministicContainersTest, unordered_to_ordered_keys) {
  constexpr std::array<int, 2u> key_values{1, 2};
  UnorderedMap<int, int> map{{key_values[0], 42}, {key_values[1], 23}};
  auto keys = unordered_to_ordered_keys(map);
  EXPECT_THAT(keys, ElementsAreArray(key_values));
}

TEST_F(DeterministicContainersTest, unordered_to_ordered_keys_multimap) {
  constexpr std::array<int, 3u> key_values{1, 1, 2};
  UnorderedMultiMap<int, int> map{
      {key_values[0], 42}, {key_values[1], 45}, {key_values[2], 23}};
  auto keys = unordered_to_ordered_keys(map);
  EXPECT_THAT(keys, ElementsAreArray(key_values));
}

TEST_F(DeterministicContainersTest, unordered_accumulate) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  auto sum = unordered_accumulate(
      map, 0, [](int a, const auto& p) { return a + p.second; });
  EXPECT_EQ(42 + 23, sum);
}

TEST_F(DeterministicContainersTest, unordered_accumulate_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 45}, {2, 23}, {2, 25}};
  auto sum = unordered_accumulate(
      map, 0, [](int a, const auto& p) { return a + p.second; });
  EXPECT_EQ(42 + 45 + 23 + 25, sum);
}

TEST_F(DeterministicContainersTest, unordered_all_of) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  EXPECT_TRUE(unordered_all_of(map, [](auto& p) { return p.second >= 23; }));
  EXPECT_FALSE(unordered_all_of(map, [](auto& p) { return p.second < 23; }));
}

TEST_F(DeterministicContainersTest, unordered_all_of_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 23}, {2, 23}, {2, 25}};
  EXPECT_TRUE(unordered_all_of(map, [](auto& p) { return p.second >= 23; }));
  EXPECT_FALSE(unordered_all_of(map, [](auto& p) { return p.second < 23; }));
}

TEST_F(DeterministicContainersTest, unordered_any_of) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  EXPECT_TRUE(unordered_any_of(map, [](auto& p) { return p.second >= 42; }));
  EXPECT_TRUE(unordered_any_of(map, [](auto& p) { return p.second < 42; }));
}

TEST_F(DeterministicContainersTest, unordered_any_of_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 45}, {2, 23}, {2, 25}};
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

TEST_F(DeterministicContainersTest, unordered_none_of_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 45}, {2, 23}, {2, 25}};
  EXPECT_FALSE(unordered_none_of(map, [](auto& p) { return p.second >= 23; }));
  EXPECT_TRUE(unordered_none_of(map, [](auto& p) { return p.second < 23; }));
  EXPECT_FALSE(unordered_none_of(map, [](auto& p) { return p.second >= 42; }));
  EXPECT_FALSE(unordered_none_of(map, [](auto& p) { return p.second < 42; }));
}

TEST_F(DeterministicContainersTest, unordered_for_each) {
  constexpr std::array<std::pair<int, int>, 2u> map_values{{{1, 42}, {2, 23}}};
  UnorderedMap<int, int> map{map_values[0], map_values[1]};
  UnorderedMap<int, int> copy;
  unordered_for_each(map, [&](auto& p) { copy.insert(p); });
  ASSERT_THAT(copy, SizeIs(2u));
  EXPECT_EQ(copy.at(map_values[0].first), map_values[0].second);
  EXPECT_EQ(copy.at(map_values[1].first), map_values[1].second);
}

TEST_F(DeterministicContainersTest, unordered_for_each_multimap) {
  constexpr std::array<std::pair<int, int>, 4u> map_values{
      {{1, 42}, {1, 45}, {2, 23}, {2, 25}}};
  UnorderedMultiMap<int, int> map{map_values[0], map_values[1], map_values[2],
                                  map_values[3]};
  UnorderedMultiMap<int, int> copy;
  unordered_for_each(map, [&](auto& p) { copy.insert(p); });
  ASSERT_THAT(copy, SizeIs(4u));
  auto iterator_key_1 = unordered_equal_range(copy, 1);
  const std::vector<std::pair<int, int>> key_1{iterator_key_1.first,
                                               iterator_key_1.second};
  EXPECT_THAT(key_1, UnorderedElementsAreArray({map_values[0], map_values[1]}));
  auto iterator_key_2 = unordered_equal_range(copy, 2);
  const std::vector<std::pair<int, int>> key_2{iterator_key_2.first,
                                               iterator_key_2.second};
  EXPECT_THAT(key_2, UnorderedElementsAreArray({map_values[2], map_values[3]}));
}

TEST_F(DeterministicContainersTest, unordered_copy) {
  constexpr std::array<std::pair<int, int>, 2u> map_values{{{1, 42}, {2, 23}}};
  UnorderedMap<int, int> map{map_values[0], map_values[1]};
  std::vector<std::pair<int, int>> copy(2);
  unordered_copy(map, copy.begin());
  EXPECT_THAT(copy, UnorderedElementsAreArray(map_values));
}

TEST_F(DeterministicContainersTest, unordered_copy_multimap) {
  constexpr std::array<std::pair<int, int>, 4u> map_values{
      {{1, 42}, {1, 45}, {2, 23}, {2, 25}}};
  UnorderedMultiMap<int, int> map{map_values[0], map_values[1], map_values[2],
                                  map_values[3]};
  std::vector<std::pair<int, int>> copy(4);
  unordered_copy(map, copy.begin());
  EXPECT_THAT(copy, UnorderedElementsAreArray(map_values));
}

TEST_F(DeterministicContainersTest, unordered_copy_if) {
  constexpr int threshold = 42;
  constexpr std::array<std::pair<int, int>, 1u> int_over_equal_threshold{
      {{1, 42}}};
  UnorderedMap<int, int> map{int_over_equal_threshold[0], {2, 23}};
  std::vector<std::pair<int, int>> copy(1);
  unordered_copy_if(map, copy.begin(),
                    [](auto& p) { return p.second >= threshold; });
  EXPECT_THAT(copy, UnorderedElementsAreArray(int_over_equal_threshold));
}

TEST_F(DeterministicContainersTest, unordered_copy_if_multimap) {
  constexpr int threshold = 42;
  constexpr std::array<std::pair<int, int>, 3u> int_over_equal_threshold{
      {{1, 42}, {1, 45}, {2, 55}}};
  UnorderedMultiMap<int, int> map{int_over_equal_threshold[0],
                                  int_over_equal_threshold[1],
                                  {1, 25},
                                  {2, 23},
                                  int_over_equal_threshold[2]};
  std::vector<std::pair<int, int>> copy(3);
  unordered_copy_if(map, copy.begin(),
                    [](auto& p) { return p.second >= threshold; });
  EXPECT_THAT(copy, UnorderedElementsAreArray(int_over_equal_threshold));
}

TEST_F(DeterministicContainersTest, unordered_erase_if) {
  constexpr int threshold = 42;
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  unordered_erase_if(map, [](auto& p) { return p.second >= threshold; });
  ASSERT_THAT(map, SizeIs(1u));
  EXPECT_EQ(2, unordered_any(map)->first);
  EXPECT_EQ(23, unordered_any(map)->second);
}

TEST_F(DeterministicContainersTest, unordered_erase_if_multimap) {
  std::array<int, 3u> removed_values{42, 45, 25};
  UnorderedMultiMap<int, int> map{{1, removed_values[0]},
                                  {1, removed_values[1]},
                                  {2, 23},
                                  {2, removed_values[2]}};
  for (const int& value : removed_values) {
    unordered_erase_if(map, [&](auto& p) { return p.second == value; });
  }
  ASSERT_THAT(map, SizeIs(1u));
  EXPECT_EQ(2, unordered_any(map)->first);
  EXPECT_EQ(23, unordered_any(map)->second);
}

TEST_F(DeterministicContainersTest, unordered_transform) {
  UnorderedMap<int, int> map{{1, 42}, {2, 23}};
  constexpr std::array<std::pair<int, int>, 2u> transformed_values{
      {{1 + 1, 42 + 1}, {2 + 1, 23 + 1}}};
  std::vector<std::pair<int, int>> copy(2);
  unordered_transform(map, copy.begin(), [](auto& p) {
    return std::make_pair(p.first + 1, p.second + 1);
  });
  EXPECT_THAT(copy, UnorderedElementsAreArray(transformed_values));
}

TEST_F(DeterministicContainersTest, unordered_transform_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 45}, {2, 23}, {2, 25}};
  constexpr std::array<std::pair<int, int>, 4u> transformed_values{
      {{1 + 1, 42 + 1}, {1 + 1, 45 + 1}, {2 + 1, 23 + 1}, {2 + 1, 25 + 1}}};
  std::vector<std::pair<int, int>> copy(4);
  unordered_transform(map, copy.begin(), [](auto& p) {
    return std::make_pair(p.first + 1, p.second + 1);
  });
  EXPECT_THAT(copy, UnorderedElementsAreArray(transformed_values));
}

TEST_F(DeterministicContainersTest, insert_unordered_iterable) {
  constexpr std::array<std::pair<int, int>, 2u> map_values{{{1, 42}, {2, 23}}};
  UnorderedMap<int, int> map{map_values[0], map_values[1]};
  UnorderedMap<int, int> copy;
  insert_unordered_iterable(copy, map);
  std::vector<std::pair<int, int>> copied_values;
  unordered_for_each(copy, [&](auto& p) { copied_values.push_back(p); });
  ASSERT_THAT(copy, SizeIs(2u));
  EXPECT_THAT(copied_values, UnorderedElementsAreArray(map_values));
}

TEST_F(DeterministicContainersTest, insert_unordered_iterable_multimap) {
  constexpr std::array<std::pair<int, int>, 4u> map_values{
      {{1, 42}, {1, 45}, {2, 23}, {2, 25}}};
  UnorderedMultiMap<int, int> map{map_values[0], map_values[1], map_values[2],
                                  map_values[3]};
  UnorderedMultiMap<int, int> copy;
  insert_unordered_iterable(copy, map);
  std::vector<std::pair<int, int>> copied_values;
  unordered_for_each(copy, [&](auto& p) { copied_values.push_back(p); });
  ASSERT_THAT(copy, SizeIs(4u));
  EXPECT_THAT(copied_values, UnorderedElementsAreArray(map_values));
}

TEST_F(DeterministicContainersTest, insert_unordered_iterable_vector) {
  constexpr std::array<int, 5u> set_values{{1, 11, 7, 3, 5}};
  UnorderedSet<int> set{set_values[0], set_values[1], set_values[2],
                        set_values[3], set_values[4]};
  std::vector<int> copy;
  insert_unordered_iterable(copy, copy.end(), set);
  EXPECT_THAT(copy, UnorderedElementsAreArray(set_values));
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

TEST_F(DeterministicContainersTest, unordered_min_element_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 45}, {2, 23}, {2, 25}, {3, 55}};
  auto min = unordered_min_element(
      map, [](const auto& a, const auto& b) { return a.second < b.second; });
  EXPECT_EQ(2, min->first);
  EXPECT_EQ(23, min->second);
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

TEST_F(DeterministicContainersTest, unordered_min_element_multimap_custom) {
  UnorderedMultiMap<std::string, int> map{
      {"abc", 1}, {"a", 5}, {"a", 2}, {"abcd", 3}};
  auto min = unordered_min_element(map, [](const auto& a, const auto& b) {
    return a.first.length() == b.first.length()
               ? a.second < b.second
               : a.first.length() < b.first.length();
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

TEST_F(DeterministicContainersTest, unordered_max_element_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 45}, {2, 23}, {2, 25}};
  auto max = unordered_max_element(
      map, [](const auto& a, const auto& b) { return a.second < b.second; });
  EXPECT_EQ(1, max->first);
  EXPECT_EQ(45, max->second);
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
  ASSERT_THAT(bag, SizeIs(3u));
  EXPECT_FALSE(bag.empty());

  UnorderedBag<int> empty_bag;
  ASSERT_THAT(empty_bag, SizeIs(0u));
  EXPECT_TRUE(empty_bag.empty());
}

TEST_F(DeterministicContainersTest, UnorderedBag_basic_operations) {
  UnorderedBag<int> bag;
  EXPECT_TRUE(bag.empty());

  bag.emplace(42);
  ASSERT_THAT(bag, SizeIs(1u));
  EXPECT_FALSE(bag.empty());

  bag.emplace(23);
  ASSERT_THAT(bag, SizeIs(2u));

  bag.emplace(15);
  bag.emplace(7);
  ASSERT_THAT(bag, SizeIs(4u));
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
  constexpr std::array<int, 3u> transformed_values{1 * 2, 2 * 2, 3 * 2};
  std::vector<int> result(3);
  unordered_transform(bag, result.begin(), [](int val) { return val * 2; });
  EXPECT_THAT(result, UnorderedElementsAreArray(transformed_values));
}

TEST_F(DeterministicContainersTest, UnorderedBag_unordered_copy) {
  constexpr std::array<int, 4u> bag_values{1, 2, 3, 4};
  UnorderedBag<int> bag{bag_values[0], bag_values[1], bag_values[2],
                        bag_values[3]};
  std::vector<int> copy(4);
  unordered_copy(bag, copy.begin());
  EXPECT_THAT(copy, UnorderedElementsAreArray(bag_values));
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
  constexpr int threshold = 20;
  constexpr std::array<int, 3u> int_below_equal_threshold{5, 7, 11};
  UnorderedBag<int> bag{int_below_equal_threshold[0],
                        int_below_equal_threshold[1],
                        int_below_equal_threshold[2], 42, 23};
  std::vector<int> bag_values;
  unordered_erase_if(bag, [](int x) { return x > threshold; });
  unordered_for_each(bag, [&](auto& x) { bag_values.push_back(x); });
  ASSERT_THAT(bag, SizeIs(3u));
  EXPECT_THAT(bag_values, UnorderedElementsAreArray(int_below_equal_threshold));
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
  ASSERT_THAT(bag, SizeIs(7u));
  auto ordered = unordered_to_ordered(bag, [](int a, int b) { return a < b; });
  EXPECT_THAT(ordered, ElementsAre(1, 2, 4, 5, 7, 8, 10))
      << "Output of unordered_to_ordered is expected to contain only integers "
         "that are within the range of 1 to 10 and not divisible by 3 in "
         "ascending order";
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

TEST_F(DeterministicContainersTest, unordered_find_multimap) {
  UnorderedMultiMap<int, std::string> map{
      {1, "one"}, {1, "uno"}, {1, "une"}, {2, "two"}};

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

TEST_F(DeterministicContainersTest, unordered_find_if_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 45}, {2, 23}, {2, 25}};
  auto found =
      unordered_find_if(map, [](const auto& p) { return p.second == 25; });
  EXPECT_NE(map.end(), found);
  EXPECT_EQ(2, found->first);
  EXPECT_EQ(25, found->second);

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

TEST_F(DeterministicContainersTest, unordered_find_if_not_multimap) {
  UnorderedMultiMap<int, int> map{{1, 42}, {1, 45}, {2, 23}, {2, 25}};
  auto found =
      unordered_find_if_not(map, [](const auto& p) { return p.second > 24; });
  EXPECT_NE(map.end(), found);
  EXPECT_EQ(2, found->first);
  EXPECT_TRUE(found->second <= 23);

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

TEST_F(DeterministicContainersTest, unordered_multimap_equal_range) {
  UnorderedMultiMap<int, std::string> map{
      {1, "one"}, {1, "uno"}, {1, "une"}, {2, "two"}};
  auto range = map.equal_range(1);
  EXPECT_NE(range.first, range.second);

  auto empty_range = map.equal_range(3);
  EXPECT_EQ(empty_range.first, empty_range.second);
}

TEST_F(DeterministicContainersTest, unordered_multimap_unordered_equal_range) {
  UnorderedMultiMap<int, std::string> map{
      {1, "one"}, {1, "uno"}, {1, "une"}, {2, "two"}};
  auto range = unordered_equal_range(map, 1);
  std::vector<std::string> values;
  for (auto it = range.first; it != range.second; ++it) {
    values.push_back(it->second);
  }
  ASSERT_THAT(values, UnorderedElementsAre("one", "une", "uno"));

  auto empty_range = unordered_equal_range(map, 3);
  EXPECT_EQ(empty_range.first, empty_range.second);
}
