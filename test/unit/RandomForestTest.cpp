/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RandomForest.h"

#include <sstream>

#include <gtest/gtest.h>

namespace random_forest {

struct RandomForestTestHelper {
  MethodContextContext context{};
  MethodContext caller{context, {3.5, boost::none, 7}};
  MethodContext callee{context, {2.5, boost::none, 5}};
};

struct RandomForestTest : public testing::Test {};

TEST_F(RandomForestTest, deserialize_basic_fail) {
  EXPECT_ANY_THROW(Forest::deserialize("(test)"));
}

TEST_F(RandomForestTest, deserialize_forest) {
  EXPECT_ANY_THROW(Forest::deserialize("(forest)"));
}

TEST_F(RandomForestTest, deserialize_acc) {
  auto simple_acc_true1 = Forest::deserialize("(forest (acc 1 0))");
  EXPECT_EQ(simple_acc_true1.dump(), "(acc 1)");
  auto simple_acc_true2 = Forest::deserialize("(forest (acc 1 1))");
  EXPECT_EQ(simple_acc_true2.dump(), "(acc 1)");
  auto simple_acc_false = Forest::deserialize("(forest (acc 13 14))");
  EXPECT_EQ(simple_acc_false.dump(), "(acc 0)");

  EXPECT_ANY_THROW(Forest::deserialize("(forest (acc))"));
  EXPECT_ANY_THROW(Forest::deserialize("(forest (acc 0))"));
  EXPECT_ANY_THROW(Forest::deserialize("(forest (acc 0 0))"));
  EXPECT_ANY_THROW(Forest::deserialize("(forest (acc 0 0 1))"));

  EXPECT_ANY_THROW(Forest::deserialize("(forest (acc 0a 0))"));
  EXPECT_ANY_THROW(Forest::deserialize("(forest (acc 0 0b))"));
}

TEST_F(RandomForestTest, deserialize_feat) {
  EXPECT_ANY_THROW(Forest::deserialize("(forest (feat))"));
  EXPECT_ANY_THROW(Forest::deserialize("(forest (feat \"caller_hits\"))"));
  EXPECT_ANY_THROW(Forest::deserialize("(forest (feat a))"));
  EXPECT_ANY_THROW(Forest::deserialize("(forest (feat \"caller_hits\" b))"));
  EXPECT_ANY_THROW(
      Forest::deserialize("(forest (feat \"caller_hits\" 1.5 (acc 0 1)))"));
  EXPECT_ANY_THROW(Forest::deserialize(
      "(forest (feat \"caller_hits\" 1.5 (acc 0 1) (acc)))"));
  EXPECT_ANY_THROW(Forest::deserialize(
      "(forest (feat \"caller_hits\" a (acc 0 1) (acc 1 0)))"));
  EXPECT_ANY_THROW(
      Forest::deserialize("(forest (feat a 1.5 (acc 0 1) (acc 1 0)))"));

  auto forest = Forest::deserialize(
      "(forest (feat \"caller_hits\" 5.5 (acc 1 0) (acc 0 1)))");
  EXPECT_EQ(forest.dump(), "(feat \"caller_hits\" 5.500000 (acc 1) (acc 0))");
}

TEST_F(RandomForestTest, accept_acc) {
  RandomForestTestHelper mfth{};
  auto& context = mfth.context;
  auto& caller = mfth.caller;
  auto& callee = mfth.callee;

  EXPECT_TRUE(Forest::deserialize("(forest (acc 1 0))").accept(caller, callee));
  EXPECT_FALSE(
      Forest::deserialize("(forest (acc 0 1))").accept(caller, callee));
}

TEST_F(RandomForestTest, accept_feat_caller) {
  RandomForestTestHelper mfth{};
  auto& context = mfth.context;
  auto& caller = mfth.caller;
  auto& callee = mfth.callee;

  // The values intentionally are more than one unit apart so that the loop
  // below also ensures caller vs callee works.

  caller.m_regs = 7;
  caller.m_insns = 7;
  caller.m_blocks = 7;
  caller.m_edges = 7;
  caller.m_num_loops = 7;
  caller.m_deepest_loop = 7;

  callee.m_regs = 5;
  callee.m_insns = 5;
  callee.m_blocks = 5;
  callee.m_edges = 5;
  callee.m_num_loops = 5;
  callee.m_deepest_loop = 5;

  std::vector<std::string> feature_suffixes = {
      "_hits",  "_insns",     "_regs",         "_blocks",
      "_edges", "_num_loops", "_deepest_loop",
  };

  for (bool is_caller : std::vector<bool>{true, false}) {
    const float threshold = is_caller ? 7 : 5;
    std::string prefix = is_caller ? "caller" : "callee";
    for (const std::string& suffix : feature_suffixes) {
      for (int32_t i = -1; i < 2; ++i) {
        std::string serialized = "(forest (feat \"" + prefix + suffix + "\" " +
                                 std::to_string(threshold + i) +
                                 " (acc 1 0) (acc 0 1)))";
        auto forest = Forest::deserialize(serialized);
        // Test is "val <= threshold".
        EXPECT_EQ(forest.accept(caller, callee), i >= 0) << serialized;
      }
    }
  }
}

} // namespace random_forest
