/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
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

using ::testing::UnorderedElementsAre;

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

TEST_F(MatchFlowTest, InstructionGraph) {
  // Use a loop to test that the analysis will terminate in such cases.
  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (:L)
    (add-int v0 v0 v1)
    (goto :L)
  ))");

  cfg::ScopedCFG cfg{code.get()};

  using namespace detail;
  std::vector<Constraint> constraints;

  constraints.emplace_back(insn_matcher(m::add_int_()));
  constraints.emplace_back(insn_matcher(m::const_()));

  // First operand is constrained by the first constraint (i.e. itself), second
  // operand is constrained by second constraint (the const).
  constraints[0].srcs = {{0, 1}};

  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  IRInstruction* const_0 = mies[0].insn;
  IRInstruction* const_1 = mies[1].insn;
  IRInstruction* add_int = mies[2].insn;

  ASSERT_EQ(const_0->opcode(), OPCODE_CONST);
  ASSERT_EQ(const_0->get_literal(), 0);

  ASSERT_EQ(const_1->opcode(), OPCODE_CONST);
  ASSERT_EQ(const_1->get_literal(), 1);

  ASSERT_EQ(add_int->opcode(), OPCODE_ADD_INT);

  auto graph = instruction_graph(*cfg, constraints, 0);

  EXPECT_THAT(graph.inbound(0, add_int),
              UnorderedElementsAre(DataFlowGraph::Edge(0, 0, add_int),
                                   DataFlowGraph::Edge(1, 1, const_1)));

  EXPECT_FALSE(graph.has_node(1, const_0));
  EXPECT_TRUE(graph.has_node(1, const_1));
}

TEST_F(MatchFlowTest, InstructionGraphNoFlowConstraint) {
  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (add-int v0 v0 v1)
  ))");

  cfg::ScopedCFG cfg{code.get()};

  using namespace detail;
  std::vector<Constraint> constraints;

  constraints.emplace_back(insn_matcher(m::add_int_()));
  constraints[0].srcs = {{NO_LOC, 1}};

  constraints.emplace_back(insn_matcher(m::const_()));

  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  IRInstruction* const_1 = mies[1].insn;
  IRInstruction* add_int = mies[2].insn;

  ASSERT_EQ(const_1->opcode(), OPCODE_CONST);
  ASSERT_EQ(const_1->get_literal(), 1);

  ASSERT_EQ(add_int->opcode(), OPCODE_ADD_INT);

  auto graph = instruction_graph(*cfg, constraints, 0);

  EXPECT_THAT(graph.inbound(0, add_int),
              UnorderedElementsAre(DataFlowGraph::Edge(1, 1, const_1)));

  EXPECT_TRUE(graph.has_node(1, const_1));
}

TEST_F(MatchFlowTest, InstructionGraphTransitiveFailure) {
  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (sub-int v0 v1 v0)
    (add-int v0 v0 v1)
  ))");

  cfg::ScopedCFG cfg{code.get()};

  using namespace detail;
  std::vector<Constraint> constraints;

  constraints.emplace_back(insn_matcher(m::add_int_()));
  constraints[0].srcs = {{1, 2}};

  constraints.emplace_back(insn_matcher(m::sub_int_()));
  constraints[1].srcs = {{2, 2}};

  constraints.emplace_back(
      insn_matcher(m::const_(m::has_literal(m::equals<int64_t>(1)))));

  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  IRInstruction* const_1 = mies[1].insn;
  IRInstruction* sub_int = mies[2].insn;
  IRInstruction* add_int = mies[3].insn;

  ASSERT_EQ(const_1->opcode(), OPCODE_CONST);
  ASSERT_EQ(const_1->get_literal(), 1);

  ASSERT_EQ(sub_int->opcode(), OPCODE_SUB_INT);
  ASSERT_EQ(add_int->opcode(), OPCODE_ADD_INT);

  auto graph = instruction_graph(*cfg, constraints, 0);

  EXPECT_THAT(graph.inbound(0, add_int),
              UnorderedElementsAre(DataFlowGraph::Edge(0, 1, sub_int),
                                   DataFlowGraph::Edge(1, 2, const_1)));

  // Even though its flow constraints aren't met, the output from instruction
  // graph will return it because it is only concerned with reachability
  // and instruction constraints.
  EXPECT_THAT(graph.inbound(1, sub_int),
              UnorderedElementsAre(DataFlowGraph::Edge(0, 2, const_1)));

  EXPECT_TRUE(graph.has_node(2, const_1));
}

} // namespace
} // namespace mf
