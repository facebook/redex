/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRAssembler.h"
#include "RedexTest.h"
#include "Show.h"
#include "SwitchMethodPartitioning.h"

class SwitchPartitioningTest : public RedexTest {};

TEST_F(SwitchPartitioningTest, if_chains) {
  auto code1 = assembler::ircode_from_string(R"(
    (
      (load-param v1)
      (const v0 1)
      (if-eq v1 v0 :if-1)
      (const v0 2)
      (if-eq v1 v0 :if-2)
      (const v1 48)
      (return v1)
      (:if-2)
      (const v1 47)
      (return v1)
      (:if-1)
      (const v1 46)
      (return v1)
    )
  )");
  auto smp1 = SwitchMethodPartitioning::create(code1.get(),
                                               /* verify_default_case */ false);
  ASSERT_NE(smp1, boost::none);
  const auto& key_to_block1 = smp1->get_key_to_block();
  EXPECT_EQ(key_to_block1.size(), 2);

  auto code2 = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (switch v0 (:case-1 :case-2))
      (const v1 48)
      (return v1)
      (:case-1 1)
      (const v1 46)
      (return v1)
      (:case-2 2)
      (const v1 47)
      (return v1)
    )
  )");
  auto smp2 = SwitchMethodPartitioning::create(code2.get(),
                                               /* verify_default_case */ false);
  ASSERT_NE(smp2, boost::none);
  const auto& key_to_block2 = smp2->get_key_to_block();
  EXPECT_EQ(key_to_block2.size(), 2);
  for (size_t key = 1; key <= 2; key++) {
    auto* block1 = key_to_block1.at(key);
    auto* block2 = key_to_block2.at(key);
    EXPECT_TRUE(block1->structural_equals(block2)) << key << " : \n"
                                                   << show(block1) << "v.s.\n"
                                                   << show(block2);
  }
}
