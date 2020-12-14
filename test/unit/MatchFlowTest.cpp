/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <iterator>
#include <vector>

#include "IRAssembler.h"
#include "IRInstruction.h"
#include "Match.h"
#include "MatchFlow.h"
#include "MatchFlowDetail.h"
#include "RedexTest.h"
#include "ScopedCFG.h"

namespace mf {
namespace {

class MatchFlowTest : public RedexTest {};

using test_range = result_t::range<std::vector<IRInstruction*>::const_iterator>;

TEST_F(MatchFlowTest, EmptyRange) {
  auto empty = test_range::empty();
  EXPECT_EQ(0, std::distance(empty.begin(), empty.end()));
}

TEST_F(MatchFlowTest, RangeUnique) {
  auto add = std::make_unique<IRInstruction>(OPCODE_ADD_INT);
  auto sub = std::make_unique<IRInstruction>(OPCODE_SUB_INT);

  std::vector<IRInstruction*> zero;
  std::vector<IRInstruction*> one{add.get()};
  std::vector<IRInstruction*> two{add.get(), sub.get()};

  test_range rzero{zero.cbegin(), zero.cend()};
  test_range rone{one.cbegin(), one.cend()};
  test_range rtwo{two.cbegin(), two.cend()};

  EXPECT_EQ(rzero.unique(), nullptr);
  EXPECT_EQ(rone.unique(), add.get());
  EXPECT_EQ(rtwo.unique(), nullptr);
}

TEST_F(MatchFlowTest, Empty) {
  flow_t f;

  auto add = f.insn(m::add_int_());
  add.src(0, add);

  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (:L)
    (add-int v0 v0 v1)
    (goto :L)
  ))");

  cfg::ScopedCFG cfg{code.get()};

  auto res = f.find(*cfg, add);

  auto insns = res.matching(add);
  EXPECT_EQ(insns.begin(), insns.end());
}

TEST_F(MatchFlowTest, InstructionConstraintAnalysis) {
  // Use a loop to test that the analysis will terminate in such cases.
  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (:L)
    (add-int v0 v0 v1)
    (goto :L)
  ))");

  cfg::ScopedCFG cfg{code.get()};
  cfg->calculate_exit_block();

  using namespace detail;
  std::vector<Constraint> constraints;

  constraints.emplace_back(insn_matcher(m::add_int_()));
  constraints.emplace_back(insn_matcher(m::const_()));

  // First operand is constrained by the first constraint (i.e. itself), second
  // operand is constrained by second constraint (the const).
  constraints[0].srcs = {{0, 1}};

  InstructionConstraintAnalysis analysis{*cfg, constraints, 0};
  analysis.run({});

  for (auto* block : cfg->blocks()) {
    auto env = analysis.get_entry_state_at(block);
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }

      auto* insn = it->insn;
      analysis.analyze_instruction(insn, &env);
      if (insn->opcode() == OPCODE_ADD_INT) {
        auto r1_obligations = env.get(1);
        ASSERT_FALSE(r1_obligations.is_bottom());
        ASSERT_FALSE(r1_obligations.is_top());
        EXPECT_EQ(r1_obligations.size(), 1);
        EXPECT_EQ(*r1_obligations.elements().begin(), Obligation(0, insn, 1));

        auto r0_obligations = env.get(0);
        ASSERT_FALSE(r0_obligations.is_bottom());
        ASSERT_FALSE(r0_obligations.is_top());
        EXPECT_EQ(r0_obligations.size(), 1);
        EXPECT_EQ(*r0_obligations.elements().begin(), Obligation(0, insn, 0));
      }
    }
  }
}

} // namespace
} // namespace mf
