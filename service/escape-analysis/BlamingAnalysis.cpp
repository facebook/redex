/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlamingAnalysis.h"

using namespace ir_analyzer;

namespace {

/*
 * Returns the register that `insn` puts its result into, assuming it has one.
 * (This function will fail if the instruction has no destination).
 */
reg_t dest(const IRInstruction* insn) {
  return insn->has_move_result_any() ? RESULT_REGISTER : insn->dest();
}

} // namespace

namespace local_pointers {
namespace blaming {

FixpointIterator::FixpointIterator(
    const cfg::ControlFlowGraph& cfg,
    std::unordered_set<const IRInstruction*> allocators)
    : ir_analyzer::BaseIRAnalyzer<Environment>(cfg),
      m_allocators(std::move(allocators)) {}

void FixpointIterator::analyze_instruction(const IRInstruction* insn,
                                           Environment* env) const {
  escape_heap_referenced_objects(insn, env);

  auto op = insn->opcode();
  if (op == OPCODE_RETURN_OBJECT) {
    env->set_may_escape(insn->src(0), insn);
  } else if (is_allocator(insn)) {
    env->set_fresh_pointer(dest(insn), insn);
  } else {
    default_instruction_handler(insn, env);
  }
}

BlameStore::Domain analyze_escapes(
    cfg::ControlFlowGraph& cfg,
    std::unordered_set<const IRInstruction*> allocators) {
  if (!cfg.exit_block()) {
    cfg.calculate_exit_block();
  }

  blaming::FixpointIterator fp{cfg, std::move(allocators)};
  fp.run({});

  return fp.get_exit_state_at(cfg.exit_block()).get_store();
}

} // namespace blaming
} // namespace local_pointers
