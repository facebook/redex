/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SwitchMethodPartitioning.h"

#include <boost/variant.hpp>
#include <queue>

#include "ConstantPropagationAnalysis.h"
#include "ControlFlow.h"

namespace cp = constant_propagation;

namespace {

/**
 * The "determining" register is the one that holds the value that decides which
 * case block we go to.
 */
reg_t find_determining_reg(
    cfg::Block* b, const cp::intraprocedural::FixpointIterator& fixpoint) {
  auto last_it = b->get_last_insn();
  always_assert_log(last_it != b->end(), "non-leaf nodes should not be empty");
  auto last = last_it->insn;
  always_assert_log(opcode::is_branch(last->opcode()),
                    "%s is not a branch instruction", SHOW(last));
  boost::optional<reg_t> candidate_reg;
  auto srcs_size = last->srcs_size();
  if (srcs_size == 1) {
    // SWITCH_* or IF_*Z
    return last->src(0);
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
      const auto& domain = env.get<SignedConstantDomain>(reg);
      if (domain.is_top()) {
        return false;
      }
      return domain.get_constant() != boost::none;
    };
    bool left_is_known = is_known(left_reg);
    bool right_is_known = is_known(right_reg);
    // The determining register should have an unknown value at the end of this
    // block, whereas the other register should have a known constant
    if (!left_is_known && right_is_known) {
      return left_reg;
    } else if (left_is_known && !right_is_known) {
      return right_reg;
    } else {
      not_reached_log(
          "Could not find determining register (unexpected structure of "
          "non-leaf node)\n%s",
          SHOW(b));
    }
  }
  not_reached_log(
      "Could not find determining register (unrecognized last instruction)\n%s",
      SHOW(b));
}

/**
 * Fill `prologue_blocks` and derive the structure of the method. Return
 * Return a `reg_t` of the register if the method is a switch or if-else
 * chain over said register.
 * Return a `bool` otherwise, which is `true` iff the prologue ends with a
 * throw (indicative of a an "empty switch" that only throws
 * `IllegalArgumentException`).
 */
boost::variant<reg_t, bool> compute_prologue_blocks(
    cfg::ControlFlowGraph* cfg,
    const cp::intraprocedural::FixpointIterator& fixpoint,
    bool verify_default_case,
    std::vector<cfg::Block*>& prologue_blocks) {
  for (const cfg::Block* b : cfg->blocks()) {
    always_assert_log(!b->is_catch(),
                      "SwitchMethodPartitioning does not support methods with "
                      "catch blocks. %zu has a catch block in %s",
                      b->id(), SHOW(*cfg));
  }

  // First, add all the prologue blocks that forma a linear chain before the
  // case block selection blocks (a switch or an if-else tree) begin.
  for (cfg::Block* b = cfg->entry_block(); b != nullptr;
       b = b->goes_to_only_edge()) {
    prologue_blocks.push_back(b);
  }

  {
    auto last_prologue_block = prologue_blocks.back();
    auto last_prologue_insn_it = last_prologue_block->get_last_insn();
    always_assert(last_prologue_insn_it != last_prologue_block->end());
    auto last_prologue_insn = last_prologue_insn_it->insn;
    // If this method was compiled from a default-case-only switch, there will
    // be no branch opcode -- the method will always throw an
    // IllegalArgumentException, or return when the switch is optimized.
    auto op = last_prologue_insn->opcode();
    always_assert_log(!verify_default_case || opcode::is_branch(op) ||
                          opcode::is_a_return(op) || op == OPCODE_THROW,
                      "%s in %s", SHOW(last_prologue_insn), SHOW(*cfg));

    if (!opcode::is_branch(op)) {
      return opcode::is_throw(op);
    } else if (opcode::is_switch(op)) {
      // switch or if-else tree. Not both.
      return last_prologue_insn->src(0);
    }
  }

  // Handle a tree of if statements in the prologue. d8 emits this
  // when it would be smaller than a switch statement. The non-leaf nodes of the
  // tree are prologue blocks. The leaf nodes of the tree are case blocks.
  //
  // For example:
  //   load-param v0
  //   const v1 1
  //   if-eq v0 v1 CASE_1
  //   goto EXIT_BLOCK      ; or return
  //   const v1 2
  //   if-eq v0 v1 CASE_2
  //   goto EXIT_BLOCK      ; or return
  //   ...
  //
  // Traverse the tree in starting at the end of the linear chain of prologue
  // blocks and stopping before we reach a leaf.
  boost::optional<reg_t> determining_reg = boost::none;
  std::queue<cfg::Block*> to_visit;
  to_visit.push(prologue_blocks.back());
  while (!to_visit.empty()) {
    auto b = to_visit.front();
    to_visit.pop();

    // Leaf nodes have 0 or 1 successors (return or goto the epilogue blocks).
    // Throw edges are disallowed.
    if (b->succs().size() >= 2) {
      // The linear check above and this tree check both account for the
      // top-most node in the tree. Make sure we don't duplicate it
      if (b != prologue_blocks.back()) {
        prologue_blocks.push_back(b);

        // Verify there aren't extra instructions in here that we may lose track
        // of
        for (const auto& mie : InstructionIterable(b)) {
          auto insn = mie.insn;
          auto op = insn->opcode();
          always_assert_log(opcode::is_a_const(op) ||
                                opcode::is_a_conditional_branch(op),
                            "Unexpected instruction in if-else tree %s",
                            SHOW(insn));
        }
      }
      for (auto succ : b->succs()) {
        to_visit.push(succ->target());
      }

      // Make sure all blocks agree on which register is the determiner
      reg_t candidate_reg = ::find_determining_reg(b, fixpoint);
      if (determining_reg == boost::none) {
        determining_reg = candidate_reg;
      } else {
        always_assert_log(
            *determining_reg == candidate_reg,
            "Conflict: which register are we switching on? %d != %d in %s",
            *determining_reg, candidate_reg, SHOW(*cfg));
      }
    }
  }
  always_assert_log(determining_reg != boost::none,
                    "Couldn't find determining register in %s", SHOW(*cfg));
  return *determining_reg;
}

} // namespace

boost::optional<SwitchMethodPartitioning> SwitchMethodPartitioning::create(
    IRCode* code, bool verify_default_case) {
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  // Note that a single-case switch can be compiled as either a switch opcode or
  // a series of if-* opcodes. We can use constant propagation to handle these
  // cases uniformly: to determine the case key, we use the inferred value of
  // the operand to the branching opcode in the successor blocks.
  cp::intraprocedural::FixpointIterator fixpoint(
      cfg, cp::ConstantPrimitiveAnalyzer());
  fixpoint.run(ConstantEnvironment());

  std::vector<cfg::Block*> prologue_blocks;
  auto res = compute_prologue_blocks(&cfg, fixpoint, verify_default_case,
                                     prologue_blocks);

  reg_t determining_reg{0};
  if (res.which() == 1) {
    if (!boost::get<bool>(res)) {
      return boost::none;
    }
  } else {
    determining_reg = boost::get<reg_t>(res);
  }

  // Find all the outgoing edges from the prologue blocks
  std::vector<cfg::Edge*> cases;
  for (const cfg::Block* prologue : prologue_blocks) {
    for (cfg::Edge* e : prologue->succs()) {
      if (std::find(prologue_blocks.begin(), prologue_blocks.end(),
                    e->target()) == prologue_blocks.end()) {
        cases.push_back(e);
      }
    }
  }

  if (res.which() == 1 && !cases.empty()) {
    // This does not look like the simple throw function we expect!
    return boost::none;
  }

  std::unordered_map<int32_t, cfg::Block*> key_to_block;
  for (auto edge : cases) {
    auto case_block = edge->target();
    auto env = fixpoint.get_entry_state_at(case_block);
    auto case_key = env.get<SignedConstantDomain>(determining_reg);
    if (case_key.is_top() && verify_default_case) {
      auto last_insn_it = case_block->get_last_insn();
      always_assert_log(last_insn_it != case_block->end() &&
                            last_insn_it->insn->opcode() == OPCODE_THROW,
                        "Could not determine key for block that does not look "
                        "like it throws an IllegalArgumentException: %zu in %s",
                        case_block->id(), SHOW(cfg));
    } else if (!case_key.is_top()) {
      const auto& c = case_key.get_constant();
      if (c != boost::none) {
        key_to_block[*c] = case_block;
      } else {
        // handle multiple case keys that map to a single block
        const auto& edge_case_key = edge->case_key();
        // Constant-propagation might infer NEZ for the default case; tolerate
        // that.
        if (edge_case_key == boost::none &&
            case_key.interval() == sign_domain::Interval::NEZ) {
          auto last_insn_it = case_block->get_last_insn();
          if (last_insn_it != case_block->end() &&
              last_insn_it->insn->opcode() == OPCODE_THROW) {
            // Looks like a block that throws an IllegalArgumentException
            continue;
          }
        }
        always_assert(edge->type() == cfg::EDGE_BRANCH);
        always_assert(edge_case_key != boost::none);
        key_to_block[*edge_case_key] = case_block;
      }
    }
  }

  return SwitchMethodPartitioning(code, std::move(prologue_blocks),
                                  std::move(key_to_block));
}
