/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CompactPointerVector.h"
#include "RedexTest.h"

#include <gtest/gtest.h>

class CompactPointerVectorTest : public RedexTest {
 protected:
  CompactPointerVectorTest() {}

  void SetUp() override {
    // Reset vector before each test
    vec.clear();
  }

  // Common test data
  int a = 1;
  int b = 2;
  int c = 3;
  int d = 4;

  CompactPointerVector<int*> vec;
};

TEST_F(CompactPointerVectorTest, EmptyVector) {
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_EQ(vec.begin(), vec.end());
}

TEST_F(CompactPointerVectorTest, SingleElement) {
  vec.push_back(&a);
  EXPECT_FALSE(vec.empty());
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(vec.at(0), &a);
  EXPECT_EQ(vec.front(), &a);
  EXPECT_EQ(vec.back(), &a);
  EXPECT_EQ(*vec.begin(), &a);
  EXPECT_EQ(*(vec.end() - 1), &a);
}

TEST_F(CompactPointerVectorTest, TwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(vec[1], &b);
  EXPECT_EQ(vec.front(), &a);
  EXPECT_EQ(vec.back(), &b);
}

TEST_F(CompactPointerVectorTest, MultipleElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(vec[1], &b);
  EXPECT_EQ(vec[2], &c);
  EXPECT_EQ(vec.front(), &a);
  EXPECT_EQ(vec.back(), &c);
}

TEST_F(CompactPointerVectorTest, PopBackFromThreeElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  vec.pop_back();
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(vec[1], &b);
}

TEST_F(CompactPointerVectorTest, PopBackFromTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.pop_back();
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], &a);
}

TEST_F(CompactPointerVectorTest, PopBackFromOneElement) {
  vec.push_back(&a);
  vec.pop_back();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
}

TEST_F(CompactPointerVectorTest, IteratorRangeNoElements) {
  std::vector<int*> expected = {};
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), expected.begin()));
}

TEST_F(CompactPointerVectorTest, IteratorRangeOneElement) {
  vec.push_back(&a);
  std::vector<int*> expected = {&a};
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), expected.begin()));
}

TEST_F(CompactPointerVectorTest, IteratorRangeTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  std::vector<int*> expected = {&a, &b};
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), expected.begin()));
}

TEST_F(CompactPointerVectorTest, IteratorRangeThreeElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  std::vector<int*> expected = {&a, &b, &c};
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), expected.begin()));
}

TEST_F(CompactPointerVectorTest, CopyConstructorOneElement) {
  vec.push_back(&a);
  CompactPointerVector<int*> copy(vec);
  EXPECT_EQ(copy.size(), 1u);
  EXPECT_EQ(copy[0], &a);
}

TEST_F(CompactPointerVectorTest, CopyConstructorTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  CompactPointerVector<int*> copy(vec);
  EXPECT_EQ(copy.size(), 2u);
  EXPECT_EQ(copy[0], &a);
  EXPECT_EQ(copy[1], &b);
}

TEST_F(CompactPointerVectorTest, CopyConstructorThreeElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  CompactPointerVector<int*> copy(vec);
  EXPECT_EQ(copy.size(), 3u);
  EXPECT_EQ(copy[0], &a);
  EXPECT_EQ(copy[1], &b);
  EXPECT_EQ(copy[2], &c);
}

TEST_F(CompactPointerVectorTest, CopyAssignment) {
  vec.push_back(&a);
  vec.push_back(&b);
  CompactPointerVector<int*> copy;
  copy = vec;
  EXPECT_EQ(copy.size(), 2u);
  EXPECT_EQ(copy[0], &a);
  EXPECT_EQ(copy[1], &b);
}

// Test copy assignment self-assignment does not corrupt state
TEST_F(CompactPointerVectorTest, CopyAssignmentSelf) {
  vec.push_back(&a);
  vec = vec; // Self-assignment
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], &a);
}

TEST_F(CompactPointerVectorTest, MoveConstructorOneElement) {
  vec.push_back(&a);
  CompactPointerVector<int*> moved(std::move(vec));
  EXPECT_EQ(moved.size(), 1u);
  EXPECT_EQ(moved[0], &a);
}

TEST_F(CompactPointerVectorTest, MoveConstructorTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  CompactPointerVector<int*> moved(std::move(vec));
  EXPECT_EQ(moved.size(), 2u);
  EXPECT_EQ(moved[0], &a);
  EXPECT_EQ(moved[1], &b);
}

TEST_F(CompactPointerVectorTest, MoveConstructorThreeElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  CompactPointerVector<int*> moved(std::move(vec));
  EXPECT_EQ(moved.size(), 3u);
  EXPECT_EQ(moved[0], &a);
  EXPECT_EQ(moved[1], &b);
  EXPECT_EQ(moved[2], &c);
}

// Test move assignment self-assignment does not corrupt state
TEST_F(CompactPointerVectorTest, MoveAssignmentSelf) {
  vec.push_back(&a);
  vec = std::move(vec); // Self move-assignment
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], &a);
}

TEST_F(CompactPointerVectorTest, MoveAssignment) {
  vec.push_back(&a);
  vec.push_back(&b);
  CompactPointerVector<int*> moved;
  moved = std::move(vec);
  EXPECT_EQ(moved.size(), 2u);
  EXPECT_EQ(moved[0], &a);
  EXPECT_EQ(moved[1], &b);
}

// Test erase with empty range returns the first iterator unchanged
TEST_F(CompactPointerVectorTest, EraseEmptyRange) {
  vec.push_back(&a);
  auto* it = vec.erase(vec.begin(), vec.begin());
  EXPECT_EQ(it, vec.begin());
  EXPECT_EQ(vec.size(), 1u);
}

TEST_F(CompactPointerVectorTest, EraseSingleElement) {
  vec.push_back(&a);
  auto* it = vec.erase(vec.begin(), vec.end());
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(it, vec.end());
}

TEST_F(CompactPointerVectorTest, EraseFirstOfTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  auto* it = vec.erase(vec.begin(), vec.begin() + 1);
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], &b);
  EXPECT_EQ(it, vec.begin());
}

TEST_F(CompactPointerVectorTest, EraseSecondOfTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  auto* it = vec.erase(vec.begin() + 1, vec.end());
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(it, vec.end());
}

TEST_F(CompactPointerVectorTest, EraseFromManyToTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  auto* it = vec.erase(vec.begin() + 2, vec.end());
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(vec[1], &b);
  EXPECT_EQ(it, vec.end());
}

TEST_F(CompactPointerVectorTest, EraseFromManyToEmpty) {
  vec.push_back(&a);
  vec.push_back(&b);
  auto* it = vec.erase(vec.begin(), vec.end());
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(it, vec.end());
}

TEST_F(CompactPointerVectorTest, EraseMiddle) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  auto* it = vec.erase(vec.begin() + 1, vec.begin() + 2);
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(vec[1], &c);
  EXPECT_EQ(*it, &c);
}

TEST_F(CompactPointerVectorTest, ClearEmpty) {
  vec.clear();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
}

TEST_F(CompactPointerVectorTest, ClearSingleElement) {
  vec.push_back(&a);
  vec.clear();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
}

TEST_F(CompactPointerVectorTest, ClearTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.clear();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
}

TEST_F(CompactPointerVectorTest, ClearMany) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  vec.clear();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
}

// Test to_vector on empty vector returns empty std::vector
TEST_F(CompactPointerVectorTest, ToVectorEmpty) {
  auto v = vec.to_vector();
  EXPECT_TRUE(v.empty());
}

// Test to_vector on single element vector returns vector with one element
TEST_F(CompactPointerVectorTest, ToVectorSingleElement) {
  vec.push_back(&a);
  auto v = vec.to_vector();
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0], &a);
}

TEST_F(CompactPointerVectorTest, ToVectorTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  auto v = vec.to_vector();
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], &a);
  EXPECT_EQ(v[1], &b);
}

TEST_F(CompactPointerVectorTest, ToVectorMany) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  auto v = vec.to_vector();
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[0], &a);
  EXPECT_EQ(v[1], &b);
  EXPECT_EQ(v[2], &c);
}

// Test shrink_to_fit and capacity for empty vector
TEST_F(CompactPointerVectorTest, ShrinkToFitAndCapacityEmpty) {
  vec.shrink_to_fit();
  EXPECT_EQ(vec.capacity(), 0u);
}

// Test shrink_to_fit and capacity for single element vector
TEST_F(CompactPointerVectorTest, ShrinkToFitAndCapacitySingleElement) {
  vec.push_back(&a);
  vec.shrink_to_fit();
  EXPECT_GE(vec.capacity(), 1u);
}

// Test shrink_to_fit and capacity for two elements vector
TEST_F(CompactPointerVectorTest, ShrinkToFitAndCapacityTwoElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.shrink_to_fit();
  EXPECT_GE(vec.capacity(), 2u);
}

// Test shrink_to_fit and capacity for three elements vector
TEST_F(CompactPointerVectorTest, ShrinkToFitAndCapacityThreeElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  vec.shrink_to_fit();
  EXPECT_GE(vec.capacity(), 3u);
}

// Test shrink_to_fit and capacity for four elements vector
TEST_F(CompactPointerVectorTest, ShrinkToFitAndCapacityFourElements) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  vec.push_back(&d);
  vec.shrink_to_fit();
  EXPECT_GE(vec.capacity(), 4u);
}

// Test shrink_to_fit and capacity after removing one element
TEST_F(CompactPointerVectorTest, ShrinkToFitAndCapacityAfterRemove) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  vec.pop_back();
  vec.shrink_to_fit();
  EXPECT_GE(vec.capacity(), 2u);
}

// Test adding nullptr to the vector
TEST_F(CompactPointerVectorTest, AddNullptr) {
  vec.push_back(nullptr);
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], nullptr);
}

// Test transition from 1 to 0 element with nullptr
TEST_F(CompactPointerVectorTest, TransitionOneToZeroWithNullptr) {
  vec.push_back(nullptr);
  vec.pop_back();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.size(), 0u);
}

// Test transition from 2 to 1 element with nullptr
TEST_F(CompactPointerVectorTest, TransitionTwoToOneWithNullptr) {
  vec.push_back(&a);
  vec.push_back(nullptr);
  vec.pop_back();
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], &a);
}
