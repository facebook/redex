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

TEST_F(CompactPointerVectorTest, IteratorRange) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  std::vector<int*> expected = {&a, &b, &c};
  EXPECT_TRUE(std::equal(vec.begin(), vec.end(), expected.begin()));
}

TEST_F(CompactPointerVectorTest, CopyConstructor) {
  vec.push_back(&a);
  vec.push_back(&b);
  CompactPointerVector<int*> copy(vec);
  EXPECT_EQ(copy.size(), 2u);
  EXPECT_EQ(copy[0], &a);
  EXPECT_EQ(copy[1], &b);
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

TEST_F(CompactPointerVectorTest, MoveConstructor) {
  vec.push_back(&a);
  vec.push_back(&b);
  CompactPointerVector<int*> moved(std::move(vec));
  EXPECT_EQ(moved.size(), 2u);
  EXPECT_EQ(moved[0], &a);
  EXPECT_EQ(moved[1], &b);
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
  auto it = vec.erase(vec.begin(), vec.begin());
  EXPECT_EQ(it, vec.begin());
  EXPECT_EQ(vec.size(), 1u);
}

TEST_F(CompactPointerVectorTest, EraseSingleElement) {
  vec.push_back(&a);
  auto it = vec.erase(vec.begin(), vec.end());
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(it, vec.end());
}

TEST_F(CompactPointerVectorTest, EraseFromManyToOne) {
  vec.push_back(&a);
  vec.push_back(&b);
  auto it = vec.erase(vec.begin() + 1, vec.end());
  EXPECT_EQ(vec.size(), 1u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(it, vec.end());
}

TEST_F(CompactPointerVectorTest, EraseFromManyToEmpty) {
  vec.push_back(&a);
  vec.push_back(&b);
  auto it = vec.erase(vec.begin(), vec.end());
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(it, vec.end());
}

TEST_F(CompactPointerVectorTest, EraseMiddle) {
  vec.push_back(&a);
  vec.push_back(&b);
  vec.push_back(&c);
  auto it = vec.erase(vec.begin() + 1, vec.begin() + 2);
  EXPECT_EQ(vec.size(), 2u);
  EXPECT_EQ(vec[0], &a);
  EXPECT_EQ(vec[1], &c);
  EXPECT_EQ(*it, &c);
}

TEST_F(CompactPointerVectorTest, Clear) {
  vec.push_back(&a);
  vec.push_back(&b);
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

TEST_F(CompactPointerVectorTest, ToVectorMany) {
  vec.push_back(&a);
  vec.push_back(&b);
  auto v = vec.to_vector();
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], &a);
  EXPECT_EQ(v[1], &b);
}
