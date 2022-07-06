/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReachingDefinitions.h"

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "IROpcode.h"
#include "RedexTest.h"

namespace {

class ReachingDefinitionsTest : public RedexTest {};

TEST_F(ReachingDefinitionsTest, TracksMoveResults) {
  auto code = assembler::ircode_from_string(R"((
    (new-instance "Ljava/lang/Object;")
    (move-result-pseudo-object v0)
    (return-void)
  ))");

  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  reaching_defs::MoveAwareFixpointIterator fp_iter(cfg);
  fp_iter.run({});

  auto env = fp_iter.get_exit_state_at(cfg.exit_block());
  auto defs = env.get(0);

  ASSERT_FALSE(defs.is_top());
  ASSERT_EQ(1, defs.size());
  EXPECT_EQ(OPCODE_NEW_INSTANCE, (*defs.elements().begin())->opcode());

  code->clear_cfg();
}

TEST_F(ReachingDefinitionsTest, ResetMoveResult) {
  auto code = assembler::ircode_from_string(R"((
    (new-instance "Ljava/lang/Object;")
    (move-result-pseudo-object v0)
    (move-result-pseudo-object v1)
    (return-void)
  ))");

  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  reaching_defs::MoveAwareFixpointIterator fp_iter(cfg);
  fp_iter.run({});

  auto env = fp_iter.get_exit_state_at(cfg.exit_block());

  auto v0_defs = env.get(0);
  ASSERT_FALSE(v0_defs.is_top());
  ASSERT_EQ(1, v0_defs.size());
  EXPECT_EQ(OPCODE_NEW_INSTANCE, (*v0_defs.elements().begin())->opcode());

  auto v1_defs = env.get(1);
  ASSERT_TRUE(v1_defs.is_top());

  code->clear_cfg();
}

} // namespace
