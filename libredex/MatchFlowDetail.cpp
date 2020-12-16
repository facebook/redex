/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MatchFlowDetail.h"

#include <algorithm>
#include <queue>

#include <boost/optional/optional.hpp>

#include "MonotonicFixpointIterator.h"
#include "PatriciaTreeSetAbstractDomain.h"

namespace mf {
namespace detail {

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

inline LocationIx node_loc(const DataFlowGraph::Node& n) {
  return std::get<0>(n);
}

inline IRInstruction* node_insn(const DataFlowGraph::Node& n) {
  return std::get<1>(n);
}

struct SpartaDFG {
  using Graph = DataFlowGraph;
  using NodeId = DataFlowGraph::Node;
  using EdgeId = DataFlowGraph::Edge;
  using NodeHash = boost::hash<NodeId>;

  static NodeId entry(const Graph&) { return NodeId{NO_LOC, nullptr}; }

  static const std::vector<EdgeId>& predecessors(const Graph& graph,
                                                 const NodeId& n) {
    return graph.inbound(node_loc(n), node_insn(n));
  }

  static const std::vector<EdgeId>& successors(const Graph& graph,
                                               const NodeId& n) {
    return graph.outbound(node_loc(n), node_insn(n));
  }

  static NodeId source(const Graph&, const EdgeId& e) { return e.from; }

  static NodeId target(const Graph&, const EdgeId& e) { return e.to; }
};

// Types for InconsistentDFGNodesAnalysis' (IDN) Abstract State.
using IDNDomain = sparta::PatriciaTreeSetAbstractDomain<IRInstruction*>;
using IDNPartition =
    sparta::PatriciaTreeMapAbstractPartition<LocationIx, IDNDomain>;

struct InconsistentDFGNodesAnalysis
    : public sparta::MonotonicFixpointIterator<SpartaDFG,
                                               IDNPartition,
                                               SpartaDFG::NodeHash> {
  using Base = sparta::
      MonotonicFixpointIterator<SpartaDFG, IDNPartition, SpartaDFG::NodeHash>;

  using NodeId = SpartaDFG::NodeId;
  using EdgeId = SpartaDFG::EdgeId;

  explicit InconsistentDFGNodesAnalysis(
      const DataFlowGraph& dfg, const std::vector<Constraint>& constraints)
      : Base(dfg, dfg.size()), m_dfg(dfg), m_constraints(constraints) {}

  void analyze_node(const NodeId& n, IDNPartition* part) const override {
    if (node_loc(n) == NO_LOC) {
      // Entrypoint doesn't require analysis.
      return;
    }

    // Nothing to do if the node is already considered inconsistent.
    if (part->get(node_loc(n)).contains(node_insn(n))) {
      return;
    }

    auto& srcs = m_constraints.at(node_loc(n)).srcs;
    std::vector<size_t> consistent_edges(srcs.size());

    // Sources without constraints are implicitly consistent.
    for (size_t i = 0; i < srcs.size(); ++i) {
      if (srcs[i].loc == NO_LOC) {
        consistent_edges.at(i)++;
      }
    }

    for (const auto& e : m_dfg.inbound(node_loc(n), node_insn(n))) {
      if (node_loc(e.from) == NO_LOC) {
        // Skip sentinel nodes.
        continue;
      }

      // The abstract partition tracks inconsistent nodes, if it does not
      // contain the source node, it is treated as a consistent source.
      if (!part->get(node_loc(e.from)).contains(node_insn(e.from))) {
        consistent_edges.at(e.src)++;
      }
    }

    bool is_consistent = std::all_of(consistent_edges.begin(),
                                     consistent_edges.end(),
                                     [](size_t count) { return count > 0; });

    if (!is_consistent) {
      part->update(node_loc(n), [&n](const IDNDomain& dom) {
        if (dom.is_bottom()) {
          return IDNDomain(node_insn(n));
        } else {
          auto cpy = dom;
          cpy.add(node_insn(n));
          return cpy;
        }
      });
    }
  }

  IDNPartition analyze_edge(const EdgeId&,
                            const IDNPartition& p) const override {
    return p;
  }

 private:
  const DataFlowGraph& m_dfg;
  const std::vector<Constraint>& m_constraints;
};

} // namespace

InstructionMatcher::~InstructionMatcher() = default;

void InstructionConstraintAnalysis::analyze_instruction(
    IRInstruction* insn, ICAPartition* env) const {

  // Curried lambda (o: Obligation) -> (dom: ICADomain) -> ICADomain returning
  // a copy of dom with o added.
  const auto add_obligation = [](Obligation o) {
    return [o = std::move(o)](const ICADomain& dom) {
      if (dom.is_bottom()) {
        return ICADomain(o);
      }

      auto cpy = dom;
      cpy.add(o);
      return cpy;
    };
  };

  // Propagate data-flow constraints if the instruction constraint at loc
  // matches for the instruction being analyzed.
  const auto propagate = [this, insn, env, add_obligation](LocationIx loc) {
    const auto& constraint = m_constraints.at(loc);
    if (!constraint.insn_matcher->matches(insn)) {
      return;
    }

    size_t srcs = insn->srcs_size();
    size_t edges = constraint.srcs.size();
    for (size_t ix = 0; ix < std::min(srcs, edges); ++ix) {
      if (constraint.srcs[ix].loc == NO_LOC) {
        continue;
      }

      reg_t src = insn->src(ix);
      env->update(src, add_obligation({loc, insn, ix}));
    }
  };

  if (auto d = dest(insn)) {
    // Instructions stomp their destination registers, so no other instruction
    // can satisfy these obligations along this trace.
    ICADomain obligations;
    env->update(*d, [&obligations](const ICADomain& dom) {
      obligations = dom;
      return ICADomain::bottom();
    });

    always_assert(!obligations.is_top());
    if (!obligations.is_bottom()) {
      for (const Obligation& o : obligations.elements()) {
        auto to_loc = std::get<0>(o);
        auto to_src = std::get<2>(o);

        const auto& from_src = m_constraints.at(to_loc).srcs.at(to_src);
        propagate(from_src.loc);

        if (opcode::is_a_move(insn->opcode())) {
          if (from_src.alias == AliasFlag::alias) {
            env->update(insn->src(0), add_obligation(o));
          }
        }
      }
    }
  }

  propagate(m_root);
}

DataFlowGraph::DataFlowGraph() {
  // Add the sentinel node, for pointing to entrypoints.
  add_node(NO_LOC, nullptr);
}

size_t DataFlowGraph::size() const {
  // Subtract 1 for the sentinel node.
  return m_adjacencies.size() - 1;
}

bool DataFlowGraph::has_node(LocationIx loc, IRInstruction* insn) const {
  return m_adjacencies.count({loc, insn});
}

void DataFlowGraph::add_node(LocationIx loc, IRInstruction* insn) {
  (void)m_adjacencies[{loc, insn}];
}

void DataFlowGraph::add_entrypoint(LocationIx loc, IRInstruction* insn) {
  add_edge(NO_LOC, nullptr, NO_SRC, loc, insn);
}

void DataFlowGraph::add_edge(LocationIx lfrom,
                             IRInstruction* ifrom,
                             src_index_t src,
                             LocationIx lto,
                             IRInstruction* ito) {
  Node f{lfrom, ifrom};
  Node t{lto, ito};
  Edge e{f, src, t};
  m_adjacencies[f].out.push_back(e);
  m_adjacencies[t].in.push_back(e);
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

Locations DataFlowGraph::locations(LocationIx root) const {
  Locations locations;

  // Exist to avoid creating a temporary for potentially unnecessary
  // insertions.
  const Source no_src;
  const Sources no_srcs;

  // Ensures `node` exists in `locations`.  Returns a pointer to the node's
  // Sources if it was added as a consequence of this call.  Returns nullptr
  // otherwise.
  const auto insert_node = [&locations, &no_srcs](const Node& n) -> Sources* {
    auto loc = node_loc(n);
    auto* insn = node_insn(n);

    auto& insns = locations.at(loc);
    if (!insns) {
      insns = std::make_unique<Instructions>();
    }

    // C++17: Use try_emplace to avoid copy.
    auto res = insns->emplace(insn, no_srcs);

    // Return pointer to value if this is the first time the node was inserted
    return res.second ? &res.first->second : nullptr;
  };

  std::queue<Node> frontier;

  // (1) Determine roots and count locations
  size_t locs = 0;
  for (auto& adj : m_adjacencies) {
    auto& n = adj.first;
    auto loc = node_loc(n);

    if (loc == root) {
      frontier.push(n);
    }

    if (loc != NO_LOC && loc >= locs) {
      locs = loc + 1;
    }
  }

  // (2) Reserve space for all locations
  locations.resize(locs);

  // (3) Traverse graph from roots, adding nodes.
  for (; !frontier.empty(); frontier.pop()) {
    auto& n = frontier.front();

    if (auto* srcs = insert_node(n)) {
      auto& in = m_adjacencies.find(n)->second.in;

      for (auto& e : in) {
        if (e.src == NO_SRC) {
          continue;
        }

        if (e.src >= srcs->size()) {
          srcs->resize(e.src + 1, no_src);
        }

        srcs->at(e.src).push_back(node_insn(e.from));
        frontier.push(e.from);
      }
    }
  }

  return locations;
}

void DataFlowGraph::propagate_flow_constraints(
    const std::vector<Constraint>& constraints) {

  InconsistentDFGNodesAnalysis analysis{*this, constraints};
  analysis.run({});

  // (1) Erase inconsistent nodes.
  for (auto it = m_adjacencies.begin(), end = m_adjacencies.end(); it != end;) {
    auto& node = it->first;
    auto part = analysis.get_exit_state_at(node);

    if (part.get(node_loc(node)).contains(node_insn(node))) {
      it = m_adjacencies.erase(it);
    } else {
      ++it;
    }
  }

  // (2) Erase edges from/to inconsistent nodes from consistent ones.
  for (auto& adj : m_adjacencies) {
    auto& in = adj.second.in;
    auto in_rm = std::remove_if(in.begin(), in.end(), [this](Edge& e) {
      return !m_adjacencies.count(e.from);
    });

    auto& out = adj.second.out;
    auto out_rm = std::remove_if(out.begin(), out.end(), [this](Edge& e) {
      return !m_adjacencies.count(e.to);
    });

    in.erase(in_rm, in.end());
    out.erase(out_rm, out.end());
  }
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
    if (loc == NO_LOC) {
      return false;
    }

    auto& constraint = constraints.at(loc);
    if (!constraint.insn_matcher->matches(insn)) {
      return false;
    }

    bool obligation_free = std::all_of(
        constraint.srcs.begin(),
        constraint.srcs.end(),
        [](const Constraint::Src& src) { return src.loc == NO_LOC; });

    if (obligation_free) {
      graph.add_entrypoint(loc, insn);
    } else {
      graph.add_node(loc, insn);
    }

    return true;
  };

  // Check whether `insn` could serve as the operand implied by the obligation:
  //   o = (to_loc, to_insn, to_src)
  // and add the appropriate edge to the graph if so.
  const auto test_edge = [&](Obligation o, IRInstruction* insn) {
    auto to_loc = std::get<0>(o);
    auto to_insn = std::get<1>(o);
    auto to_src = std::get<2>(o);

    auto from_loc = constraints.at(to_loc).srcs.at(to_src).loc;
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
        const ICADomain& obligations = env.get(*d);
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
