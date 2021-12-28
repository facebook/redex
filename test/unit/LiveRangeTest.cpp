/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "IRAssembler.h"
#include "LiveRange.h"
#include "RedexTest.h"
#include "ScopedCFG.h"

class LiveRangeTest : public RedexTest {};

TEST_F(LiveRangeTest, LiveRangeSingleBlock) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (return-void)
    )
  )");
  code->set_registers_size(1);

  live_range::renumber_registers(code.get(), /* width_aware */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v1)
     (check-cast v1 "Ljava/lang/Object;")
     (move-result-pseudo-object v2)
     (return-void)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
  EXPECT_EQ(code->get_registers_size(), 3);
}

TEST_F(LiveRangeTest, LiveRange) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (if-eq v0 v0 :if-true-label)

     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)

     (:if-true-label)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (return-void)
    )
  )");

  live_range::renumber_registers(code.get(), /* width_aware */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v1)
     (check-cast v1 "Ljava/lang/Object;")
     (move-result-pseudo-object v2)
     (if-eq v2 v2 :if-true-label)

     (const v3 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v4)
     (check-cast v4 "Ljava/lang/Object;")
     (move-result-pseudo-object v2)

     (:if-true-label)
     (check-cast v2 "Ljava/lang/Object;")
     (move-result-pseudo-object v5)
     (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
  EXPECT_EQ(code->get_registers_size(), 6);
}

TEST_F(LiveRangeTest, WidthAwareLiveRange) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const-wide v0 0)
     (sput-wide v0 "LFoo;.bar:I")
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (return-void)
    )
  )");

  live_range::renumber_registers(code.get(), /* width_aware */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const-wide v1 0)
     (sput-wide v1 "LFoo;.bar:I")
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v3) ; skip v2 since we have a wide value in v1
     (check-cast v3 "Ljava/lang/Object;")
     (move-result-pseudo-object v4)
     (return-void)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
  EXPECT_EQ(code->get_registers_size(), 5);
}

TEST_F(LiveRangeTest, testDefUseChainSingleDefinition) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 1)
      (if-eq v1 v1 :if-true)
      (return v0)
      (:if-true)
      (return v0)
    )
  )");

  using namespace live_range;

  cfg::ScopedCFG cfg(code.get());
  auto du_chains = Chains(*cfg).get_def_use_chains();

  EXPECT_EQ(du_chains.size(), 2);

  auto it = InstructionIterable(*cfg).begin();
  auto next_insn = [&it]() { return (*it++).insn; };

  IRInstruction* const_v0 = next_insn();
  IRInstruction* const_v1 = next_insn();
  IRInstruction* if_eq = next_insn();
  IRInstruction* first_return = next_insn();
  IRInstruction* second_return = next_insn();

  ASSERT_TRUE(du_chains.count(const_v0));
  ASSERT_TRUE(du_chains.count(const_v1));

  EXPECT_THAT(du_chains[const_v0],
              ::testing::UnorderedElementsAre(Use{first_return, 0},
                                              Use{second_return, 0}));
  EXPECT_THAT(du_chains[const_v1],
              ::testing::UnorderedElementsAre(Use{if_eq, 0}, Use{if_eq, 1}));
}

TEST_F(LiveRangeTest, testDefUseChainMultiDefinition) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eq v0 v0 :if-true)
      (const v1 1)
      (goto :end)
      (:if-true)
      (const v1 2)
      (:end)
      (move v2 v1)
      (return-void)
    )
  )");

  using namespace live_range;

  cfg::ScopedCFG cfg(code.get());
  auto du_chains = Chains(*cfg).get_def_use_chains();

  auto it = InstructionIterable(*cfg).begin();
  auto next_insn = [&it]() { return (*it++).insn; };

  IRInstruction* const_v0 = next_insn();
  EXPECT_EQ(const_v0->opcode(), OPCODE_CONST);
  next_insn(); // discard if-eq
  IRInstruction* const_v1_1 = next_insn();
  EXPECT_EQ(const_v1_1->opcode(), OPCODE_CONST);

  IRInstruction* const_v1_2 = next_insn();
  EXPECT_EQ(const_v1_2->opcode(), OPCODE_CONST);

  IRInstruction* move = next_insn();
  EXPECT_EQ(move->opcode(), OPCODE_MOVE);

  ASSERT_TRUE(du_chains.count(const_v1_1));
  ASSERT_TRUE(du_chains.count(const_v1_2));

  EXPECT_THAT(du_chains[const_v1_1],
              ::testing::UnorderedElementsAre(Use{move, 0}));
  EXPECT_THAT(du_chains[const_v1_2],
              ::testing::UnorderedElementsAre(Use{move, 0}));
}
