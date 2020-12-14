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

} // namespace detail
} // namespace mf
