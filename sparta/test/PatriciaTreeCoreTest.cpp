/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PatriciaTreeCore.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <type_traits>

using namespace sparta;

using Key = size_t;

using PatriciaTreeNode =
    pt_core::PatriciaTreeNode<Key, pt_core::SimpleValue<std::string>>;
using PatriciaTreeLeaf = PatriciaTreeNode::LeafType;
using PatriciaTreeBranch = PatriciaTreeNode::BranchType;

using PatriciaTreeNodeEmpty =
    pt_core::PatriciaTreeNode<Key, pt_core::EmptyValue>;
using PatriciaTreeLeafEmpty = PatriciaTreeNodeEmpty::LeafType;
using PatriciaTreeBranchEmpty = PatriciaTreeNodeEmpty::BranchType;

TEST(PatriciaTreeCoreTest, noVirtualTable) {
  // A vtable adds both size and latency, avoid using it.
  EXPECT_FALSE(std::is_polymorphic_v<PatriciaTreeNode>);
  EXPECT_FALSE(std::is_polymorphic_v<PatriciaTreeLeaf>);
  EXPECT_FALSE(std::is_polymorphic_v<PatriciaTreeBranch>);
  EXPECT_FALSE(std::is_polymorphic_v<PatriciaTreeNodeEmpty>);
  EXPECT_FALSE(std::is_polymorphic_v<PatriciaTreeLeafEmpty>);
  EXPECT_FALSE(std::is_polymorphic_v<PatriciaTreeBranchEmpty>);
}

TEST(PatriciaTreeCoreTest, nodeSizes) {
  // The base node is always the same size.
  EXPECT_EQ(sizeof(PatriciaTreeNodeEmpty), sizeof(PatriciaTreeNode));

  // The leaf node is smaller for the empty value.
  EXPECT_LT(sizeof(PatriciaTreeLeafEmpty), sizeof(PatriciaTreeLeaf));

  // The branch node is bigger for the empty value, for the hash.
  EXPECT_GT(sizeof(PatriciaTreeBranchEmpty), sizeof(PatriciaTreeBranch));
}
