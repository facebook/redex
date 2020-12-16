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
namespace detail {
inline bool operator==(const DataFlowGraph::Edge& e,
                       const DataFlowGraph::Edge& f) {
  return e.from == f.from && e.src == f.src && e.to == f.to;
}

DataFlowGraph::Edge Edge(LocationIx from_loc,
                         IRInstruction* from_insn,
                         src_index_t src,
                         LocationIx to_loc,
                         IRInstruction* to_insn) {
  return DataFlowGraph::Edge({from_loc, from_insn}, src, {to_loc, to_insn});
}
} // namespace detail

namespace {

using ::testing::Contains;
using ::testing::UnorderedElementsAre;

// Assert that the MIE contains an instruction whose opcode is OPCODE and then
// return a pointer to that instruction.
#define ASSERT_INSN(IDENT, MIE, OPCODE) \
  IRInstruction* IDENT = (MIE).insn;    \
  ASSERT_EQ((IDENT)->opcode(), (OPCODE))

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

TEST_F(MatchFlowTest, MultipleResults) {
  flow_t f;

  auto const_int = f.insn(m::const_());

  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (:L)
    (add-int v0 v0 v1)
    (goto :L)
  ))");

  cfg::ScopedCFG cfg{code.get()};
  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(const_0, mies[0], OPCODE_CONST);
  ASSERT_INSN(const_1, mies[1], OPCODE_CONST);

  auto res = f.find(*cfg, const_int);
  auto insns = res.matching(const_int);

  std::vector<IRInstruction*> vinsns{insns.begin(), insns.end()};
  EXPECT_THAT(vinsns, UnorderedElementsAre(const_0, const_1));
}

TEST_F(MatchFlowTest, Cycle) {
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
  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(add_int, mies[2], OPCODE_ADD_INT);

  auto res = f.find(*cfg, add);

  auto insns = res.matching(add);
  EXPECT_EQ(insns.unique(), add_int);
}

TEST_F(MatchFlowTest, MatchingNotRoot) {
  flow_t f;

  auto lit = f.insn(m::const_());
  auto add = f.insn(m::add_int_()).src(0, lit);
  auto sub = f.insn(m::sub_int_()).src(0, add).src(1, add);

  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (const v2 2)
    (add-int v3 v0 v2)
    (add-int v4 v1 v2)
    (add-int v5 v2 v2)
    (sub-int v6 v3 v4)
  ))");

  cfg::ScopedCFG cfg{code.get()};
  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(const_0, mies[0], OPCODE_CONST);
  ASSERT_INSN(const_1, mies[1], OPCODE_CONST);

  auto res = f.find(*cfg, sub);

  auto insns = res.matching(lit);
  std::vector<IRInstruction*> vinsns{insns.begin(), insns.end()};
  EXPECT_THAT(vinsns, UnorderedElementsAre(const_0, const_1));
}

TEST_F(MatchFlowTest, MatchingNotRootDiamond) {
  flow_t f;

  auto lit = f.insn(m::const_());
  auto add = f.insn(m::add_int_()).src(0, lit);
  auto sub = f.insn(m::sub_int_()).src(0, add).src(1, add);

  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (const v1 1)
    (add-int v2 v0 v1)
    (add-int v3 v0 v1)
    (add-int v4 v1 v1)
    (sub-int v5 v2 v3)
  ))");

  cfg::ScopedCFG cfg{code.get()};
  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(const_0, mies[0], OPCODE_CONST);

  auto res = f.find(*cfg, sub);

  auto insns = res.matching(lit);
  std::vector<IRInstruction*> vinsns{insns.begin(), insns.end()};
  EXPECT_THAT(vinsns, UnorderedElementsAre(const_0));
}

TEST_F(MatchFlowTest, OnlyMatchingSource) {
  flow_t f;

  auto lit = f.insn(m::const_());
  auto add = f.insn(m::add_int_()).src(0, lit);

  auto code = assembler::ircode_from_string(R"((
    (const v0 0)
    (add-int v0 v0 v0)
    (const v1 1)
    (add-int v1 v1 v1)
  ))");

  cfg::ScopedCFG cfg{code.get()};
  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(const_0, mies[0], OPCODE_CONST);
  ASSERT_INSN(add_int_0, mies[1], OPCODE_ADD_INT);
  ASSERT_INSN(const_1, mies[2], OPCODE_CONST);
  ASSERT_INSN(add_int_1, mies[3], OPCODE_ADD_INT);

  auto res = f.find(*cfg, add);

  auto srcs_0 = res.matching(add, add_int_0, 0);
  EXPECT_EQ(srcs_0.unique(), const_0);

  auto srcs_1 = res.matching(add, add_int_1, 0);
  EXPECT_EQ(srcs_1.unique(), const_1);

  auto consts = res.matching(lit);
  std::vector<IRInstruction*> vconsts{consts.begin(), consts.end()};
  EXPECT_THAT(vconsts, UnorderedElementsAre(const_0, const_1));
}

TEST_F(MatchFlowTest, MultipleMatchingSource) {
  flow_t f;

  auto lit = f.insn(m::const_());
  auto add = f.insn(m::add_int_()).src(0, lit);

  auto code = assembler::ircode_from_string(R"((
    (load-param v0)
    (if-eqz v0 :else)
    (const v0 0)
    (goto :end)
    (:else)
    (const v0 1)
    (:end)
    (add-int v0 v0 v0)
    (const v1 2)
    (add-int v1 v1 v1)
    (return-void)
  ))");

  cfg::ScopedCFG cfg{code.get()};
  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(const_0, mies[2], OPCODE_CONST);
  ASSERT_INSN(const_1, mies[3], OPCODE_CONST);
  ASSERT_INSN(add_int_0, mies[4], OPCODE_ADD_INT);
  ASSERT_INSN(const_2, mies[5], OPCODE_CONST);
  ASSERT_INSN(add_int_1, mies[6], OPCODE_ADD_INT);

  auto res = f.find(*cfg, add);

  auto srcs_0 = res.matching(add, add_int_0, 0);
  std::vector<IRInstruction*> vsrcs_0{srcs_0.begin(), srcs_0.end()};
  EXPECT_THAT(vsrcs_0, UnorderedElementsAre(const_0, const_1));

  auto srcs_1 = res.matching(add, add_int_1, 0);
  EXPECT_EQ(srcs_1.unique(), const_2);

  auto consts = res.matching(lit);
  std::vector<IRInstruction*> vconsts{consts.begin(), consts.end()};
  EXPECT_THAT(vconsts, UnorderedElementsAre(const_0, const_1, const_2));
}

TEST_F(MatchFlowTest, DFGSize) {
  using namespace detail;
  DataFlowGraph graph;
  EXPECT_EQ(0, graph.size());

  graph.add_node(0, nullptr);
  EXPECT_EQ(1, graph.size());

  graph.add_node(0, nullptr);
  EXPECT_EQ(1, graph.size());
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
  constraints[0].srcs = {
      {0, AliasFlag::dest, QuantFlag::exists},
      {1, AliasFlag::dest, QuantFlag::exists},
  };

  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(const_0, mies[0], OPCODE_CONST);
  ASSERT_INSN(const_1, mies[1], OPCODE_CONST);
  ASSERT_INSN(add_int, mies[2], OPCODE_ADD_INT);

  ASSERT_EQ(const_0->get_literal(), 0);
  ASSERT_EQ(const_1->get_literal(), 1);

  auto graph = instruction_graph(*cfg, constraints, 0);

  EXPECT_THAT(graph.inbound(0, add_int),
              UnorderedElementsAre(Edge(0, add_int, 0, 0, add_int),
                                   Edge(1, const_1, 1, 0, add_int)));

  EXPECT_THAT(graph.outbound(NO_LOC, nullptr),
              Contains(Edge(NO_LOC, nullptr, NO_SRC, 1, const_1)));

  EXPECT_FALSE(graph.has_node(1, const_0));

  ASSERT_TRUE(graph.has_node(0, add_int));
  ASSERT_TRUE(graph.has_node(1, const_1));
  ASSERT_EQ(graph.size(), 2);

  graph.propagate_flow_constraints(constraints);

  EXPECT_TRUE(graph.has_node(0, add_int));
  EXPECT_TRUE(graph.has_node(1, const_1));
  EXPECT_EQ(graph.size(), 2);
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
  constraints[0].srcs = {
      {NO_LOC, AliasFlag::dest, QuantFlag::exists},
      {1, AliasFlag::dest, QuantFlag::exists},
  };

  constraints.emplace_back(insn_matcher(m::const_()));

  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(const_1, mies[1], OPCODE_CONST);
  ASSERT_INSN(add_int, mies[2], OPCODE_ADD_INT);
  ASSERT_EQ(const_1->get_literal(), 1);

  auto graph = instruction_graph(*cfg, constraints, 0);

  EXPECT_THAT(graph.inbound(0, add_int),
              UnorderedElementsAre(Edge(1, const_1, 1, 0, add_int)));

  EXPECT_THAT(graph.outbound(NO_LOC, nullptr),
              Contains(Edge(NO_LOC, nullptr, NO_SRC, 1, const_1)));
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
  constraints[0].srcs = {
      {1, AliasFlag::dest, QuantFlag::exists},
      {2, AliasFlag::dest, QuantFlag::exists},
  };

  constraints.emplace_back(insn_matcher(m::sub_int_()));
  constraints[1].srcs = {
      {2, AliasFlag::dest, QuantFlag::exists},
      {2, AliasFlag::dest, QuantFlag::exists},
  };

  constraints.emplace_back(
      insn_matcher(m::const_(m::has_literal(m::equals<int64_t>(1)))));

  auto ii = InstructionIterable(*cfg);
  std::vector<MethodItemEntry> mies{ii.begin(), ii.end()};

  ASSERT_INSN(const_1, mies[1], OPCODE_CONST);
  ASSERT_INSN(sub_int, mies[2], OPCODE_SUB_INT);
  ASSERT_INSN(add_int, mies[3], OPCODE_ADD_INT);
  ASSERT_EQ(const_1->get_literal(), 1);

  auto graph = instruction_graph(*cfg, constraints, 0);

  EXPECT_THAT(graph.inbound(0, add_int),
              UnorderedElementsAre(Edge(1, sub_int, 0, 0, add_int),
                                   Edge(2, const_1, 1, 0, add_int)));

  // Even though its flow constraints aren't met, the output from instruction
  // graph will return it because it is only concerned with reachability
  // and instruction constraints.
  EXPECT_THAT(graph.inbound(1, sub_int),
              UnorderedElementsAre(Edge(2, const_1, 0, 1, sub_int)));

  EXPECT_THAT(graph.outbound(NO_LOC, nullptr),
              Contains(Edge(NO_LOC, nullptr, NO_SRC, 2, const_1)));

  ASSERT_TRUE(graph.has_node(1, sub_int));
  ASSERT_TRUE(graph.has_node(0, add_int));
  graph.propagate_flow_constraints(constraints);

  EXPECT_FALSE(graph.has_node(0, add_int));
  EXPECT_FALSE(graph.has_node(1, sub_int));
  EXPECT_TRUE(graph.has_node(2, const_1));

  auto locs = graph.locations(0);

  // Although const_1 existed in the graph, it isn't reachable from a root node.
  EXPECT_EQ(locs.at(2), nullptr);
}

} // namespace
} // namespace mf
