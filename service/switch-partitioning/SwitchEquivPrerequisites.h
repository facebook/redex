/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <unordered_map>
#include <vector>

#include "ConstantPropagationAnalysis.h"
#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "IRCode.h"
#include "Trace.h"

/**
 * Adds all the prologue blocks that form a linear chain in the method to the
 * vector and returns whether or not the cfg should continue to be operated upon
 * as a potential series of if-elses.
 */
inline bool gather_linear_prologue_blocks(
    cfg::ControlFlowGraph* cfg, std::vector<cfg::Block*>* prologue_blocks) {
  for (const cfg::Block* b : cfg->blocks()) {
    if (b->is_catch()) {
      return false;
    }
  }
  for (cfg::Block* b = cfg->entry_block(); b != nullptr;
       b = b->goes_to_only_edge()) {
    prologue_blocks->push_back(b);
  }
  if (prologue_blocks->empty()) {
    return false;
  }
  auto last_prologue_block = prologue_blocks->back();
  auto last_prologue_insn_it = last_prologue_block->get_last_insn();
  if (last_prologue_insn_it == last_prologue_block->end()) {
    return false;
  }
  auto last_prologue_insn = last_prologue_insn_it->insn;
  auto op = last_prologue_insn->opcode();
  return opcode::is_branch(op);
}

/*
 * Checks possible ConstantValue domains for if they are known/supported for
 * switching over.
 */
class known_visitor : public boost::static_visitor<bool> {
 public:
  known_visitor() {}

  bool operator()(const SignedConstantDomain& dom) const {
    if (dom.is_top()) {
      return false;
    }
    return dom.get_constant() != boost::none;
  }

  bool operator()(const ConstantClassObjectDomain& dom) const {
    if (dom.is_top()) {
      return false;
    }
    return dom.get_constant() != boost::none;
  }

  template <typename Domain>
  bool operator()(const Domain&) const {
    return false;
  }
};

/**
 * The "determining" register is the one that holds the value that decides which
 * case block we go to.
 */
inline bool find_determining_reg(
    const constant_propagation::intraprocedural::FixpointIterator& fixpoint,
    cfg::Block* b,
    reg_t* determining_reg) {
  auto last_it = b->get_last_insn();
  always_assert_log(last_it != b->end(), "non-leaf nodes should not be empty");
  auto last = last_it->insn;
  always_assert_log(opcode::is_branch(last->opcode()),
                    "%s is not a branch instruction", SHOW(last));
  boost::optional<reg_t> candidate_reg;
  auto srcs_size = last->srcs_size();
  if (srcs_size == 1) {
    // SWITCH_* or IF_*Z
    *determining_reg = last->src(0);
    return true;
  } else if (srcs_size == 2) {
    // Expecting code like this:
    //   CONST vA/B X
    //   ...
    //   IF_* vA, vB
    // We want to return whichever register wasn't loaded by the constant
    // instruction. For example, on this code:
    //   CONST v0 2
    //   IF_EQ v0 v1
    // this method should return 1
    const auto& env = fixpoint.get_exit_state_at(b);
    reg_t left_reg = last->src(0);
    reg_t right_reg = last->src(1);
    const auto& is_known = [&env](reg_t reg) -> bool {
      const auto& value = env.get(reg);
      return ConstantValue::apply_visitor(known_visitor(), value);
    };
    bool left_is_known = is_known(left_reg);
    bool right_is_known = is_known(right_reg);
    // The determining register should have an unknown value at the end of this
    // block, whereas the other register should have a known constant
    if (!left_is_known && right_is_known) {
      *determining_reg = left_reg;
      return true;
    } else if (left_is_known && !right_is_known) {
      *determining_reg = right_reg;
      return true;
    } else {
      TRACE(SWITCH_EQUIV, 2,
            "Could not find determining register (unexpected structure of "
            "non-leaf node)\n%s",
            SHOW(b));
      return false;
    }
  }
  TRACE(
      SWITCH_EQUIV, 2,
      "Could not find determining register (unrecognized last instruction)\n%s",
      SHOW(b));
  return false;
}
