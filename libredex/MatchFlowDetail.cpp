/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MatchFlowDetail.h"

#include <boost/optional/optional.hpp>

namespace {

/**
 * Returns the register that holds the result of computing insn, if there is
 * one or boost::none if such a register does not exist.
 */
inline boost::optional<reg_t> dest(const IRInstruction* insn) {
  if (insn->has_move_result_any()) {
    return RESULT_REGISTER;
  } else if (insn->has_dest()) {
    return insn->dest();
  } else {
    return {};
  }
}

} // namespace

namespace mf {
namespace detail {

InstructionMatcher::~InstructionMatcher() = default;

void InstructionConstraintAnalysis::analyze_instruction(IRInstruction* insn,
                                                        Partition* env) const {

  // Propagate data-flow constraints if the instruction constraint at loc
  // matches for the instruction being analyzed.
  const auto propagate = [this, insn, env](LocationIx loc) {
    const auto& constraint = m_constraints.at(loc);
    if (!constraint.insn_matcher->matches(insn)) {
      return;
    }

    size_t srcs = insn->srcs_size();
    size_t edges = constraint.srcs.size();
    for (size_t ix = 0; ix < std::min(srcs, edges); ++ix) {
      if (constraint.srcs[ix] == NO_LOC) {
        continue;
      }

      reg_t src = insn->src(ix);
      env->update(src, [o = Obligation{loc, insn, ix}](const Domain& dom) {
        if (dom.is_bottom()) {
          return Domain(o);
        }

        auto cpy = dom;
        cpy.add(o);
        return cpy;
      });
    }
  };

  if (auto d = dest(insn)) {
    // Instructions stomp their destination registers, so no other instruction
    // can satisfy these obligations along this trace.
    Domain obligations;
    env->update(*d, [&obligations](const Domain& dom) {
      obligations = dom;
      return Domain::bottom();
    });

    always_assert(!obligations.is_top());
    if (!obligations.is_bottom()) {
      for (const Obligation& o : obligations.elements()) {
        auto to_loc = std::get<0>(o);
        auto to_src = std::get<2>(o);

        auto from_loc = m_constraints.at(to_loc).srcs.at(to_src);
        propagate(from_loc);
      }
    }
  }

  propagate(m_root);
}

bool DataFlowGraph::has_node(LocationIx loc, IRInstruction* insn) const {
  return m_adjacencies.count({loc, insn});
}

void DataFlowGraph::add_node(LocationIx loc, IRInstruction* insn) {
  (void)m_adjacencies[{loc, insn}];
}

void DataFlowGraph::add_edge(LocationIx lfrom,
                             IRInstruction* ifrom,
                             src_index_t src,
                             LocationIx lto,
                             IRInstruction* ito) {
  m_adjacencies[{lfrom, ifrom}].out.emplace_back(src, lto, ito);
  m_adjacencies[{lto, ito}].in.emplace_back(src, lfrom, ifrom);
}

const std::vector<DataFlowGraph::Edge>& DataFlowGraph::inbound(
    LocationIx loc, IRInstruction* insn) const {
  auto it = m_adjacencies.find({loc, insn});
  if (it == m_adjacencies.end()) {
    static std::vector<Edge> none;
    return none;
  }

  return it->second.in;
}

const std::vector<DataFlowGraph::Edge>& DataFlowGraph::outbound(
    LocationIx loc, IRInstruction* insn) const {
  auto it = m_adjacencies.find({loc, insn});
  if (it == m_adjacencies.end()) {
    static std::vector<Edge> none;
    return none;
  }

  return it->second.out;
}

DataFlowGraph instruction_graph(cfg::ControlFlowGraph& cfg,
                                const std::vector<Constraint>& constraints,
                                LocationIx root) {
  if (!cfg.exit_block()) {
    // The instruction constraint analysis runs backwards and so requires a
    // single exit block to start from.
    cfg.calculate_exit_block();
  }

  InstructionConstraintAnalysis analysis{cfg, constraints, root};
  analysis.run({});

  DataFlowGraph graph;

  // Check whether (loc, insn) should be in the graph, and adds it if necessary.
  // Returns a boolean indicating whether the node was added or not.
  const auto test_node = [&](LocationIx loc, IRInstruction* insn) {
    return loc != NO_LOC && constraints.at(loc).insn_matcher->matches(insn) &&
           (graph.add_node(loc, insn), true);
  };

  // Check whether `insn` could serve as the operand implied by the obligation:
  //   o = (to_loc, to_insn, to_src)
  // and add the appropriate edge to the graph if so.
  const auto test_edge = [&](Obligation o, IRInstruction* insn) {
    auto to_loc = std::get<0>(o);
    auto to_insn = std::get<1>(o);
    auto to_src = std::get<2>(o);

    auto from_loc = constraints.at(to_loc).srcs.at(to_src);
    if (!test_node(from_loc, insn)) {
      return;
    }

    graph.add_edge(from_loc, insn, to_src, to_loc, to_insn);
  };

  for (auto* block : cfg.blocks()) {
    // The obligations at the *end* of the block.
    auto env = analysis.get_entry_state_at(block);

    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }

      auto* insn = it->insn;
      test_node(root, insn);

      if (auto d = dest(insn)) {
        const Domain& obligations = env.get(*d);
        always_assert(!obligations.is_top());

        if (!obligations.is_bottom()) {
          for (const Obligation& o : obligations.elements()) {
            test_edge(o, insn);
          }
        }
      }

      analysis.analyze_instruction(insn, &env);
    }
  }

  return graph;
}

} // namespace detail
} // namespace mf
