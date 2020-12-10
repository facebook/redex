/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "IRInstruction.h"
#include "Match.h"

#include <limits>
#include <memory>
#include <vector>

namespace mf {

struct flag_t;
struct location_t;
struct result_t;

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

} // namespace detail

struct flow_t {
  /**
   * Add a new instruction constraint to this predicate.
   *
   * Returns a location_t that can be used to refer to instructions matching
   * this constraint.
   */
  template <typename M>
  location_t insn(m::match_t<IRInstruction*, M> m);

  /**
   * Search for sub-trees originating from instructions matching the constraints
   * at l, in the given control-flow graph.
   */
  result_t find(const cfg::ControlFlowGraph& cfg, location_t l) const;

 private:
  friend struct location_t;

  std::vector<detail::Constraint> m_constraints;
};

struct location_t {
  /**
   * Add a data-flow constraint:  The operand referred to by ix must be supplied
   * by an instruction matching the constraint at l.
   */
  location_t src(src_index_t ix, location_t l);

 private:
  friend struct flow_t;
  location_t(flow_t* owner, detail::LocationIx ix) : m_owner(owner), m_ix(ix) {}

  /** Access the underlying constraint */
  detail::Constraint& constraint();

  flow_t* m_owner;
  detail::LocationIx m_ix;
};

struct result_t {
  struct insn_iterator;
  struct insn_range;
  struct src_iterator;
  struct src_range;

  /**
   * Return all instructions referred to by l in these results.
   */
  insn_range matching(location_t l) const;

  /**
   * Assuming insn is referred to by l in these results, returns all the
   * instructions that could supply its ix-th operand, and satisfy the ix-th
   * data-flow constraint on l.  If insn is not matched by the constraint at l,
   * an empty range is returned.
   */
  src_range matching(location_t l, IRInstruction* insn, src_index_t ix) const;
};

template <typename M>
inline location_t flow_t::insn(m::match_t<IRInstruction*, M> insn_matcher) {
  struct Wrapper : public detail::InstructionMatcher {
    explicit Wrapper(m::match_t<IRInstruction*, M> m)
        : m_insn_matcher(std::move(m)) {}

    ~Wrapper() override = default;

    bool matches(const IRInstruction* insn) const override {
      return m_insn_matcher.matches(insn);
    }

    m::match_t<IRInstruction*, M> m_insn_matcher;
  };

  detail::LocationIx ix = m_constraints.size();
  m_constraints.emplace_back(
      std::make_unique<Wrapper>(std::move(insn_matcher)));

  return location_t{this, ix};
}

inline location_t location_t::src(src_index_t src, location_t src_constraint) {
  always_assert(m_owner != nullptr && m_owner == src_constraint.m_owner &&
                "location_t shared between flow_t instances.");

  auto& src_locs = constraint().srcs;
  if (src_locs.size() <= src) {
    src_locs.resize(src + 1, detail::NO_LOC);
  }

  src_locs[src] = src_constraint.m_ix;
  return *this;
}

inline detail::Constraint& location_t::constraint() {
  return m_owner->m_constraints[m_ix];
}

} // namespace mf
