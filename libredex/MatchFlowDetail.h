/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <cstddef>
#include <limits>
#include <memory>

#include "BaseIRAnalyzer.h"
#include "HashedSetAbstractDomain.h"
#include "IRInstruction.h"
#include "Match.h"
#include "PatriciaTreeMapAbstractPartition.h"

namespace mf {
namespace detail {

/** Used internally to refer to a location held by a flow_t. */
using LocationIx = size_t;

/** Sentinel value denoting the lack of a location. */
constexpr LocationIx NO_LOC = std::numeric_limits<size_t>::max();

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
  virtual ~InstructionMatcher() = 0;
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
  explicit Constraint(std::unique_ptr<InstructionMatcher> insn_matcher)
      : insn_matcher(std::move(insn_matcher)) {}

  // Wraps a m::match_t<IRInstruction*, M>
  std::unique_ptr<InstructionMatcher> insn_matcher;

  // References to constraints for instructions supplying values to the various
  // source operands.  This vector can contain "holes", represented by NO_LOC.
  // Such a hole implies no constraint for that instruction operand.
  std::vector<LocationIx> srcs;
};

using Obligation = std::tuple<LocationIx, IRInstruction*, src_index_t>;
using Domain =
    sparta::HashedSetAbstractDomain<Obligation, boost::hash<Obligation>>;
using Partition = sparta::PatriciaTreeMapAbstractPartition<reg_t, Domain>;

/**
 * Tracks constraints imposed on instructions based on where their results flow
 * into.
 */
struct InstructionConstraintAnalysis
    : public ir_analyzer::BaseBackwardsIRAnalyzer<Partition> {
  using Base = ir_analyzer::BaseBackwardsIRAnalyzer<Partition>;

  InstructionConstraintAnalysis(const cfg::ControlFlowGraph& cfg,
                                const std::vector<Constraint>& constraints,
                                LocationIx root)
      : Base(cfg), m_constraints(constraints), m_root(root) {}

  void analyze_instruction(IRInstruction* insn, Partition* env) const override;

 private:
  const std::vector<Constraint>& m_constraints;
  LocationIx m_root;
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

} // namespace detail
} // namespace mf
