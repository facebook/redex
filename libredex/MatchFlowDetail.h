/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sparta/HashedSetAbstractDomain.h>
#include <sparta/PatriciaTreeMapAbstractPartition.h>

#include "BaseIRAnalyzer.h"
#include "IRInstruction.h"
#include "Match.h"

namespace mf {
namespace detail {

/** Used internally to refer to a location held by a flow_t. */
using LocationIx = size_t;

/** Sentinel value denoting the lack of a location. */
constexpr LocationIx NO_LOC = std::numeric_limits<size_t>::max();

/** Sentinel value denoting the lack of a source. */
constexpr src_index_t NO_SRC = std::numeric_limits<src_index_t>::max();

enum class AliasFlag : uint16_t {
  dest,
  alias,
  result,
};

enum class QuantFlag : uint16_t {
  exists,
  forall,
  unique,
};

/**
 * Matchers, such as
 *
 *   m::match_t<IRInstruction*, M>
 *
 * present their implementation in their type (the template parameter M), with
 * potential implications on their data layout.  This interface is used to
 * interact with these instances uniformly, by hiding that implementation.  The
 * in-memory representation is hidden by a memory indirection.
 */
struct InstructionMatcher {
  virtual ~InstructionMatcher() = default;
  virtual bool matches(const IRInstruction* insn) const = 0;
};

/**
 * Hide the implementation of a m::match_t<IRInstruction*, M> by wrapping it to
 * create a std::unique_ptr<InstructionMatcher>.
 */
template <typename M>
std::unique_ptr<InstructionMatcher> insn_matcher(
    m::match_t<IRInstruction*, M> m) {
  struct Wrapper : public detail::InstructionMatcher {
    explicit Wrapper(m::match_t<IRInstruction*, M> m)
        : m_insn_matcher(std::move(m)) {}

    bool matches(const IRInstruction* insn) const override {
      return m_insn_matcher.matches(insn);
    }

    m::match_t<IRInstruction*, M> m_insn_matcher;
  };

  return std::make_unique<Wrapper>(std::move(m));
}

/**
 * An instruction constraint is composed of the predicate that the instruction
 * is expected to match, and references to any constraints on values flowing
 * into the instruction.
 *
 * Externally, location_t values reference constraints.  Internally, for a
 * particular instance of flow_t, a LocationIx value suffices.
 */
struct Constraint {

  struct Src {
    LocationIx loc;
    AliasFlag alias;
    QuantFlag quant;
  };

  explicit Constraint(std::unique_ptr<InstructionMatcher> insn_matcher)
      : insn_matcher(std::move(insn_matcher)) {}

  /**
   * Returns a reference to the source constraint for the ix-th operand.
   */
  const Src& src(src_index_t ix) const;

  /**
   * Add a source operand constraint, either at an individual index -- ix -- or
   * at all indices from lb upwards.
   */
  void add_src(src_index_t ix,
               LocationIx loc,
               AliasFlag alias,
               QuantFlag quant);
  void add_src_range(src_index_t lb,
                     LocationIx loc,
                     AliasFlag alias,
                     QuantFlag quant);

  // Wraps a m::match_t<IRInstruction*, M>
  std::unique_ptr<InstructionMatcher> insn_matcher;

 private:
  // The default source constraint to return a reference to, in case a specific
  // constraint does not exist.
  static constexpr Src UNCONSTRAINED_SRC{NO_LOC, {}, {}};

  // References to constraints for instructions supplying values to the various
  // source operands, along with any modifiers (flags) for this edge.  This
  // vector can contain "holes", represented by a Src whose loc is NO_LOC.  Such
  // a hole implies no constraint for that instruction operand.
  std::vector<Src> m_srcs;

  // Like srcs but applying to ranges of instructions. src_ranges[i] references
  // the constraint applying to instructions supplying values to operands at
  // index j, where:
  //
  // - i <= j
  // - j < k, where k is the next highest index in src_ranges.
  // - j >= srcs.size() || srcs[j] == NO_LOC
  std::map<src_index_t, Src> m_src_ranges;
};

// Types for InstructionConstraintAnalysis' (ICA) Abstract State.
using Obligation = std::tuple<LocationIx, IRInstruction*, src_index_t>;
using ICADomain =
    sparta::HashedSetAbstractDomain<Obligation, boost::hash<Obligation>>;
using ICAPartition = sparta::PatriciaTreeMapAbstractPartition<reg_t, ICADomain>;

/**
 * Tracks constraints imposed on instructions based on where their results flow
 * into.
 */
struct InstructionConstraintAnalysis
    : public ir_analyzer::BaseBackwardsIRAnalyzer<ICAPartition> {
  using Base = ir_analyzer::BaseBackwardsIRAnalyzer<ICAPartition>;

  InstructionConstraintAnalysis(const cfg::ControlFlowGraph& cfg,
                                const std::vector<Constraint>& constraints,
                                const std::unordered_set<LocationIx>& roots)
      : Base(cfg), m_constraints(constraints), m_roots(roots) {}

  void analyze_instruction(IRInstruction* insn,
                           ICAPartition* env) const override;

 private:
  const std::vector<Constraint>& m_constraints;
  const std::unordered_set<LocationIx>& m_roots;
};

/**
 * Locations represents the following nested mapping:
 *
 *   location_t ->> IRInstruction* -> src_index_t ->> IRInstruction*
 *
 * Where ->> represents a multimap.  As all results come from a single
 * flow_t instance, the location_t can be referred to by its index which is
 * just a number.  These numbers are densely packed, so the multimap is
 * represented by a vector-of-pointers-to-maps with location indices serving
 * as keys.  The pointer indirection aims to save space in the case of an
 * empty mapping.
 *
 * Similarly, source indices are densely packed for an instruction, so the
 * inner multimap is represented by a vector-of-vectors, keyed by the source
 * index.
 */
using Source = std::vector<IRInstruction*>;
using Sources = std::vector<Source>;
using Instructions = std::unordered_map<IRInstruction*, Sources>;
using Locations = std::vector<std::unique_ptr<Instructions>>;
using Order = std::unordered_map<IRInstruction*, size_t>;

/**
 * Mutable representation of a data-flow graph.  Nodes in this graph are
 * represented by (LocationIx, IRInstruction*) pairs, and edges are labelled
 * with a src_index_t.
 */
struct DataFlowGraph {
  using Node = std::tuple<LocationIx, IRInstruction*>;

  // A redundant representation where {l, i, ix, k, j} represents the edge
  //
  //  (l, i) -[ix]-> (k, j)
  //
  // to simplify referencing source nodes from edges.
  struct Edge {
    Edge(Node from, src_index_t src, Node to)
        : from{std::move(from)}, src{src}, to{std::move(to)} {}

    Node from;
    src_index_t src;
    Node to;
  };

  /** Construct an empty data-flow graph */
  DataFlowGraph();

  // DataFlowGraph is moveable and copyable
  DataFlowGraph(DataFlowGraph&&) = default;
  DataFlowGraph& operator=(DataFlowGraph&&) = default;
  DataFlowGraph(const DataFlowGraph&) = default;
  DataFlowGraph& operator=(const DataFlowGraph&) = default;

  /**
   * Number of nodes in the graph, not including the sentinel node.
   */
  size_t size() const;

  /**
   * Decides whether (loc, insn) exists as a node in this graph.
   */
  bool has_node(LocationIx loc, IRInstruction* insn) const;

  /**
   * Decides whether the src-th operand of insn, when interpreted as a loc has
   * been marked as accepting a value that does not match its constraint.
   */
  bool has_inconsistency(LocationIx loc,
                         IRInstruction* insn,
                         src_index_t src) const;
  /**
   * Returns a reference to the edges that flow in to the (loc, insn) node.
   * Edges are represented by a tuple:
   *
   *   (ix, lfrom, ifrom)
   *
   * Where `ix` is the edge label (the index of the operand) and (lfrom, ifrom)
   * is the source node.
   *
   * If !has_node(loc, insn), a reference to an empty vector is returned.
   */
  const std::vector<Edge>& inbound(LocationIx loc, IRInstruction* insn) const;

  /**
   * Returns a reference to the edges that flow out of the (loc, insn) node.
   * Edges are represented by a tuple:
   *
   *   (ix, lto, ito)
   *
   * Where `ix` is the edge label (the index of the operand) and (lto, ito) is
   * the target node.
   *
   * If !has_node(loc, insn), a reference to an empty vector is returned.
   */
  const std::vector<Edge>& outbound(LocationIx loc, IRInstruction* insn) const;

  /**
   * Copy the sub-graph flowing into roots (i.e. reachable transitively via
   * inbound edges), converting it into the Locations data structure (internal
   * representation of mf::result_t).
   */
  Locations locations(const std::unordered_set<LocationIx>& roots) const;

  /** Add (loc, insn) as a node in the graph. */
  void add_node(LocationIx loc, IRInstruction* insn);

  /**
   * Locate nodes without any inbound edges and mark them as "entry-points".
   */
  void calculate_entrypoints();

  /**
   * Add (lfrom, ifrom) -[src]-> (lto, ito) as an edge in the graph, implicitly
   * adding (lfrom, ifrom) and (lto, ito) as nodes in the graph.
   *
   * Edges are not uniqued, multiple invocations of add_edge with the same
   * parameters will result in duplicate edges in the graph.
   */
  void add_edge(LocationIx lfrom,
                IRInstruction* ifrom,
                src_index_t src,
                LocationIx lto,
                IRInstruction* ito);

  /**
   * Indicate that a value flowing into the src-th operand of insn does not
   * match the src-th flow constraint of the loc-th constraint.
   */
  void mark_inconsistent(LocationIx loc, IRInstruction* insn, src_index_t src);

  /**
   * Apply flow constraints through the data-flow graph, removing nodes whose
   * flow constraints are not met.  Removing one such node can have transitive
   * effects (i.e. make downstream nodes inconsistent).
   *
   * Guaranteed to remove the smallest set of nodes required to ensure all
   * remaining nodes are consistent with respect to each other and the supplied
   * flow constraints.
   *
   * Edges from/to removed nodes are also cleaned up.
   */
  void propagate_flow_constraints(const std::vector<Constraint>& constraints);

 private:
  struct Adjacencies {
    std::vector<Edge> in, out;
    std::unordered_set<src_index_t> inconsistent;
  };

  // Every node in the data-flow graph exists as a key in this map.  Edges,
  //
  //   (l, i) -[ix]-> (k, j)
  //
  // are accounted for in both the outbound edge list of their source as well
  // as the inbound edge list of their target:
  //
  //   m_adjacencies[(l, i)].out
  //   m_adjacencies[(k, j)].in
  //
  // A sentinel node -- (NO_LOC, nullptr) -- has sentinel outbound edges to
  // every node without (other) inbound edges.
  //
  // Each node is also associated with a set of inconsistent sources.
  std::unordered_map<Node, Adjacencies, boost::hash<Node>> m_adjacencies;
};

/**
 * Calculate the use-def graph modulo instruction constraints in `constraints`,
 * transitively reachable from instructions matching the constraint in `roots`
 * in `cfg`.
 *
 * - Nodes in the graph are (loc, insn) pairs -- an instruction and the location
 *   referring to an instruction constraint it matches.
 * - Edges (l, i) -[src]-> (k, j) indicate that the destination of instruction i
 *   flows into the src-th operand of instruction j.
 *
 * This function relies on a backward analysis, and so will calculate an exit
 * block for the supplied `cfg` if one does not already exist.
 */
DataFlowGraph instruction_graph(cfg::ControlFlowGraph& cfg,
                                const std::vector<Constraint>& constraints,
                                const std::unordered_set<LocationIx>& roots,
                                Order* order = nullptr);

inline void Constraint::add_src(src_index_t ix,
                                LocationIx loc,
                                AliasFlag alias,
                                QuantFlag quant) {
  if (m_srcs.size() <= ix) {
    m_srcs.resize(ix + 1, UNCONSTRAINED_SRC);
  }
  m_srcs[ix] = {loc, alias, quant};
}

inline void Constraint::add_src_range(src_index_t lb,
                                      LocationIx loc,
                                      AliasFlag alias,
                                      QuantFlag quant) {
  m_src_ranges[lb] = {loc, alias, quant};
}

} // namespace detail
} // namespace mf
