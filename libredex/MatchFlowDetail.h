/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <limits>
#include <memory>

#include "IRInstruction.h"

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
} // namespace mf
