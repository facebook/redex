/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SwitchMethodPartitioning.h"

#include <boost/variant.hpp>
#include <queue>

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ControlFlow.h"
#include "SwitchEquivFinder.h"
#include "SwitchEquivPrerequisites.h"

namespace cp = constant_propagation;

namespace {
// See comments in SwitchEquivFinder.h for an explanation.
constexpr size_t DEFAULT_LEAF_DUP_THRESHOLD = 50;

// Check whether, possibly at the end of a chain of gotos, the block will
// unconditionally throw.
bool throws(cfg::Block* block) {
  std::unordered_set<cfg::Block*> visited{block};
  for (; block->goes_to_only_edge(); block = block->goes_to_only_edge()) {
    if (!visited.insert(block->goes_to_only_edge()).second) {
      // non-terminating loop
      return false;
    }
  }
  auto last_insn_it = block->get_last_insn();
  return last_insn_it != block->end() &&
         last_insn_it->insn->opcode() == OPCODE_THROW;
}
} // namespace

boost::optional<SwitchMethodPartitioning> SwitchMethodPartitioning::create(
    IRCode* code, bool verify_default_case_throws) {
  code->build_cfg(/* editable */ true);
  auto cfg = &code->cfg();
  // Check for a throw only method up front. SwitchEquivFinder will not
  // represent this out of the box, so convert directly to
  // SwitchMethodPartitioning representation.
  if (throws(cfg->entry_block())) {
    TRACE(SW, 3, "Special case: method always throws");
    std::vector<cfg::Block*> entry_blocks;
    entry_blocks.emplace_back(cfg->entry_block());
    return SwitchMethodPartitioning(code, std::move(entry_blocks), {});
  }

  // Note that a single-case switch can be compiled as either a switch opcode or
  // a series of if-* opcodes. We can use constant propagation to handle these
  // cases uniformly: to determine the case key, we use the inferred value of
  // the operand to the branching opcode in the successor blocks.
  std::vector<cfg::Block*> prologue_blocks;
  if (!gather_linear_prologue_blocks(cfg, &prologue_blocks)) {
    TRACE(SW, 3, "Prologue blocks do not have expected branching");
    return boost::none;
  }
  auto fixpoint = std::make_shared<cp::intraprocedural::FixpointIterator>(
      *cfg, SwitchEquivFinder::Analyzer());
  fixpoint->run(ConstantEnvironment());
  reg_t determining_reg;
  if (!find_determining_reg(*fixpoint, prologue_blocks.back(),
                            &determining_reg)) {
    TRACE(SW, 3, "Unknown const for branching");
    return boost::none;
  }
  auto last_prologue_block = prologue_blocks.back();
  auto last_prologue_insn = last_prologue_block->get_last_insn();
  auto root_branch = cfg->find_insn(last_prologue_insn->insn);
  auto finder = std::make_unique<SwitchEquivFinder>(
      cfg, root_branch, determining_reg, DEFAULT_LEAF_DUP_THRESHOLD,
      fixpoint, SwitchEquivFinder::EXECUTION_ORDER);
  if (!finder->success() ||
      !finder->are_keys_uniform(SwitchEquivFinder::KeyKind::INT)) {
    TRACE(SW, 3, "Cannot represent method as switch equivalent");
    return boost::none;
  }

  if (verify_default_case_throws) {
    always_assert_log(finder->default_case() != boost::none,
                      "Method does not have default case");
    auto default_block = *finder->default_case();
    always_assert_log(throws(default_block), "Default case B%zu should throw",
                      default_block->id());
  }

  // Method is supported, munge into simpler format expected by callers.
  std::map<int32_t, cfg::Block*> key_to_block;
  for (auto&& [key, block] : finder->key_to_case()) {
    if (!SwitchEquivFinder::is_default_case(key)) {
      auto i = boost::get<int32_t>(key);
      key_to_block.emplace(i, block);
    }
  }
  return SwitchMethodPartitioning(code, std::move(prologue_blocks),
                                  std::move(key_to_block));
}
