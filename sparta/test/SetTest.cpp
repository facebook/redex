/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FlatSet.h"
#include "PatriciaTreeSet.h"

#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace sparta;

namespace {

std::vector<uint32_t> get_union(const std::vector<uint32_t>& a,
                                const std::vector<uint32_t>& b) {
  std::set<uint32_t> u;
  for (uint32_t x : a) {
    u.insert(x);
  }
  for (uint32_t x : b) {
    u.insert(x);
  }
  return std::vector<uint32_t>(u.begin(), u.end());
}

std::vector<uint32_t> get_intersection(const std::vector<uint32_t>& a,
                                       const std::vector<uint32_t>& b) {
  std::set<uint32_t> i, sb;
  for (uint32_t x : b) {
    sb.insert(x);
  }
  for (uint32_t x : a) {
    if (sb.count(x)) {
      i.insert(x);
    }
  }
  return std::vector<uint32_t>(i.begin(), i.end());
}

std::vector<uint32_t> get_difference(const std::vector<uint32_t>& a,
                                     const std::vector<uint32_t>& b) {
  std::set<uint32_t> d;
  for (uint32_t x : a) {
    d.insert(x);
  }
  for (uint32_t x : b) {
    d.erase(x);
  }
  return std::vector<uint32_t>(d.begin(), d.end());
}

} // namespace

template <typename Set>
class UInt32SetTest : public ::testing::Test {
 protected:
  UInt32SetTest()
      : m_rd_device(),
        m_generator(m_rd_device()),
        m_size_dist(0, 50),
        m_elem_dist(0, std::numeric_limits<uint32_t>::max()) {}

  Set generate_random_set() {
    Set s;
    size_t size = m_size_dist(m_generator);
    for (size_t i = 0; i < size; ++i) {
      s.insert(m_elem_dist(m_generator));
    }
    return s;
  }

  std::random_device m_rd_device;
  std::mt19937 m_generator;
  std::uniform_int_distribution<uint32_t> m_size_dist;
  std::uniform_int_distribution<uint32_t> m_elem_dist;
};

using UInt32Sets =
    ::testing::Types<PatriciaTreeSet<uint32_t>, FlatSet<uint32_t>>;
TYPED_TEST_CASE(UInt32SetTest, UInt32Sets);

TYPED_TEST(UInt32SetTest, basicOperations) {
  const uint32_t bigint = std::numeric_limits<uint32_t>::max();
  TypeParam s1;
  TypeParam empty_set;
  std::vector<uint32_t> elements1 = {0, 1, 2, 3, 4, 1023, bigint};

  for (uint32_t x : elements1) {
    s1.insert(x);
  }
  EXPECT_EQ(7, s1.size());
  EXPECT_THAT(s1, ::testing::UnorderedElementsAreArray(elements1));

  for (uint32_t x : elements1) {
    EXPECT_TRUE(s1.contains(x));
    EXPECT_FALSE(empty_set.contains(x));
  }
  EXPECT_FALSE(s1.contains(17));
  EXPECT_FALSE(s1.contains(1000000));

  TypeParam s2 = s1;
  std::vector<uint32_t> elements2 = {0, 2, 3, 1023};
  s2.remove(1).remove(4).remove(bigint);

  EXPECT_THAT(s1, ::testing::UnorderedElementsAreArray(elements1));

  {
    EXPECT_THAT(s2, ::testing::UnorderedElementsAreArray(elements2));
    std::ostringstream out;
    out << s2;
    EXPECT_EQ("{0, 2, 3, 1023}", out.str());
    TypeParam s_init_list({0, 2, 3, 1023});
    EXPECT_TRUE(s_init_list.equals(s2));
  }

  EXPECT_TRUE(empty_set.is_subset_of(s1));
  EXPECT_FALSE(s1.is_subset_of(empty_set));
  EXPECT_TRUE(s2.is_subset_of(s1));
  EXPECT_FALSE(s1.is_subset_of(s2));
  EXPECT_TRUE(s1.equals(s1));
  EXPECT_TRUE(empty_set.equals(empty_set));
  EXPECT_FALSE(empty_set.equals(s1));

  std::vector<uint32_t> elements3 = {2, 1023, 4096, 13001, bigint};
  TypeParam s3(elements3.begin(), elements3.end());
  TypeParam u13 = s1;
  u13.union_with(s3);
  EXPECT_TRUE(s1.is_subset_of(u13));
  EXPECT_TRUE(s3.is_subset_of(u13));
  EXPECT_FALSE(u13.is_subset_of(s1));
  EXPECT_FALSE(u13.is_subset_of(s3));
  {
    std::vector<uint32_t> union13 = get_union(elements1, elements3);
    EXPECT_THAT(u13, ::testing::UnorderedElementsAreArray(union13));
  }
  EXPECT_TRUE(s1.get_union_with(empty_set).equals(s1));
  EXPECT_TRUE(s1.get_union_with(s1).equals(s1));

  TypeParam i13 = s1;
  i13.intersection_with(s3);
  EXPECT_TRUE(i13.is_subset_of(s1));
  EXPECT_TRUE(i13.is_subset_of(s3));
  EXPECT_FALSE(s1.is_subset_of(i13));
  EXPECT_FALSE(s3.is_subset_of(i13));
  {
    std::vector<uint32_t> intersection13 =
        get_intersection(elements1, elements3);
    EXPECT_THAT(i13, ::testing::UnorderedElementsAreArray(intersection13));
  }
  EXPECT_TRUE(s1.get_intersection_with(empty_set).empty());
  EXPECT_TRUE(empty_set.get_intersection_with(s1).empty());
  EXPECT_TRUE(s1.get_intersection_with(s1).equals(s1));

  EXPECT_EQ(elements3.size(), s3.size());
  s3.clear();
  EXPECT_EQ(0, s3.size());

  std::vector<uint32_t> elements4 = {0,    1,    2,       5,     101,
                                     4096, 8137, 1234567, bigint};
  TypeParam t3(elements3.begin(), elements3.end());
  TypeParam t4(elements4.begin(), elements4.end());
  TypeParam d34 = t3;
  d34.difference_with(t4);
  EXPECT_THAT(d34, ::testing::UnorderedElementsAre(1023, 13001));

  TypeParam d43 = t4.get_difference_with(t3);
  EXPECT_THAT(d43,
              ::testing::UnorderedElementsAre(0, 1, 5, 101, 8137, 1234567));
}

TYPED_TEST(UInt32SetTest, robustness) {
  for (size_t k = 0; k < 10; ++k) {
    TypeParam s1 = this->generate_random_set();
    TypeParam s2 = this->generate_random_set();
    auto elems1 = std::vector<uint32_t>(s1.begin(), s1.end());
    auto elems2 = std::vector<uint32_t>(s2.begin(), s2.end());
    std::vector<uint32_t> ref_u12 = get_union(elems1, elems2);
    std::vector<uint32_t> ref_i12 = get_intersection(elems1, elems2);
    std::vector<uint32_t> ref_d12 = get_difference(elems1, elems2);
    TypeParam u12 = s1.get_union_with(s2);
    TypeParam i12 = s1.get_intersection_with(s2);
    TypeParam d12 = s1.get_difference_with(s2);
    EXPECT_THAT(u12, ::testing::UnorderedElementsAreArray(ref_u12))
        << "s1 = " << s1 << ", s2 = " << s2;
    EXPECT_THAT(i12, ::testing::UnorderedElementsAreArray(ref_i12))
        << "s1 = " << s1 << ", s2 = " << s2;
    EXPECT_THAT(d12, ::testing::UnorderedElementsAreArray(ref_d12))
        << "s1 = " << s1 << ", s2 = " << s2;
    EXPECT_TRUE(s1.is_subset_of(u12));
    EXPECT_TRUE(s2.is_subset_of(u12));
    EXPECT_TRUE(i12.is_subset_of(s1));
    EXPECT_TRUE(i12.is_subset_of(s2));
    EXPECT_TRUE(d12.is_subset_of(s1));
  }
}

template <typename Set>
class StringSetTest : public ::testing::Test {
 protected:
  static std::vector<std::string> string_set_to_vector(const Set& s) {
    std::vector<std::string> v;
    for (std::string* p : s) {
      v.push_back(*p);
    }
    return v;
  }
};

using StringSets =
    ::testing::Types<PatriciaTreeSet<std::string*>, FlatSet<std::string*>>;
TYPED_TEST_CASE(StringSetTest, StringSets);

TYPED_TEST(StringSetTest, setsOfPointers) {
  std::string a = "a";
  std::string b = "b";
  std::string c = "c";
  std::string d = "d";

  TypeParam s_abcd;
  s_abcd.insert(&a).insert(&b).insert(&c).insert(&d);
  EXPECT_THAT(this->string_set_to_vector(s_abcd),
              ::testing::UnorderedElementsAre("a", "b", "c", "d"));

  TypeParam s_bc = s_abcd;
  s_bc.remove(&a).remove(&d);
  EXPECT_THAT(this->string_set_to_vector(s_bc),
              ::testing::UnorderedElementsAre("b", "c"));

  TypeParam s_ab = s_abcd;
  s_ab.filter([](std::string* x) { return *x < "c"; });
  EXPECT_THAT(this->string_set_to_vector(s_ab),
              ::testing::UnorderedElementsAre("a", "b"));
  TypeParam s = s_ab;
  s.filter([](std::string* x) { return *x >= "a"; });
  EXPECT_TRUE(s.equals(s_ab));
  s.filter([](std::string* x) { return *x > "g"; });
  EXPECT_TRUE(s.empty());

  TypeParam t({&a});
  std::ostringstream out;
  out << t;
  EXPECT_EQ("{a}", out.str());
}

template <typename Set>
class UInt64SetTest : public ::testing::Test {};

using UInt64Sets =
    ::testing::Types<PatriciaTreeSet<uint64_t>, FlatSet<uint64_t>>;
TYPED_TEST_CASE(UInt64SetTest, UInt64Sets);

TYPED_TEST(UInt64SetTest, setOfUnsignedInt64) {
  TypeParam s;
  std::set<uint64_t> values = {0, 1, 2, 10, 4000000000};

  for (auto v : values) {
    s.insert(v);
  }
  EXPECT_EQ(values.size(), s.size());
  for (auto x : s) {
    EXPECT_EQ(1, values.count(x));
  }
}

TEST(PatriciaTreeSet, singleton) {
  using Set = PatriciaTreeSet<uint64_t>;
  EXPECT_EQ(Set{}.singleton(), nullptr);
  EXPECT_NE(Set{1}.singleton(), nullptr);
  EXPECT_EQ(*(Set{1}.singleton()), 1);
  EXPECT_EQ((Set{1, 2}.singleton()), nullptr);
}
