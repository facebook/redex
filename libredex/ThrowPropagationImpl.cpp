/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ThrowPropagationImpl.h"
#include "MethodUtil.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"

namespace throw_propagation_impl {

bool ThrowPropagator::try_apply(const cfg::InstructionIterator& cfg_it) {
  if (!check_if_dead_code_present_and_prepare_block(cfg_it)) {
    return false;
  }
  insert_unreachable(cfg_it);
  return true;
}

bool ThrowPropagator::will_throw_or_not_terminate_or_unreachable(
    cfg::InstructionIterator it) {
  std::unordered_set<IRInstruction*> visited{it->insn};
  while (true) {
    it = m_cfg.next_following_gotos(it);
    if (!visited.insert(it->insn).second) {
      // We found a loop
      return true;
    }
    switch (it->insn->opcode()) {
    case OPCODE_CONST:
    case OPCODE_CONST_STRING:
    case OPCODE_MOVE:
    case OPCODE_NOP:
    case OPCODE_NEW_INSTANCE:
    case OPCODE_MOVE_RESULT_OBJECT:
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
      break;
    case OPCODE_INVOKE_DIRECT: {
      auto method = it->insn->get_method();
      if (!method::is_init(method) ||
          method->get_class() != type::java_lang_RuntimeException()) {
        return false;
      }
      break;
    }
    case OPCODE_THROW:
    case IOPCODE_UNREACHABLE:
      return true;
    default:
      return false;
    }
  }
  not_reached();
};

// Helper function that checks if there's any point in doing a transformation
// (not needed if we are already going to throw or not terminate anyway),
// and it performs block splitting if needed (see comment inline for details).
bool ThrowPropagator::check_if_dead_code_present_and_prepare_block(
    const cfg::InstructionIterator& cfg_it) {
  const auto block = cfg_it.block();
  const auto it = cfg_it.unwrap();
  auto insn = it->insn;
  TRACE(TP, 4, "no return: %s", SHOW(insn));
  if (insn == block->get_last_insn()->insn) {
    if (will_throw_or_not_terminate_or_unreachable(cfg_it)) {
      // There's already code in place that will immediately and
      // unconditionally throw an exception, and thus we don't need to
      // bother rewriting the code into a throw. The main reason we are
      // doing this is to not inflate our throws_inserted statistics.
      return false;
    }
  } else {
    // When the invoke instruction isn't the last in the block, then we'll
    // need to some extra work. (Ideally, we could have just inserted our
    // throwing instructions in the middle of the existing block, but that
    // causes complications due to the possibly following and then dangling
    // move-result instruction, so we'll explicitly split the block here
    // in order to keep all invariant happy.)
    if (will_throw_or_not_terminate_or_unreachable(cfg_it)) {
      // As above, nothing to do, since an exception will be thrown anyway.
      return false;
    }
    always_assert(m_cfg.get_succ_edge_of_type(block, cfg::EDGE_THROW) ==
                  nullptr);
    m_cfg.split_block(cfg_it);
    always_assert(insn == block->get_last_insn()->insn);
  }
  return true;
};

void ThrowPropagator::insert_unreachable(
    const cfg::InstructionIterator& cfg_it) {
  const auto block = cfg_it.block();
  IRInstruction* insn = cfg_it->insn;

  if (!m_reg) {
    m_reg = m_cfg.allocate_temp();
  }
  cfg::Block* new_block = m_cfg.create_block();
  std::vector<IRInstruction*> insns{
      (new IRInstruction(OPCODE_CONST))->set_literal(0)->set_dest(*m_reg),
      (new IRInstruction(OPCODE_THROW))->set_src(0, *m_reg),
  };
  new_block->push_back(insns);
  m_cfg.copy_succ_edges_of_type(block, new_block, cfg::EDGE_THROW);
  auto existing_goto_edge = m_cfg.get_succ_edge_of_type(block, cfg::EDGE_GOTO);
  always_assert(existing_goto_edge != nullptr);
  m_cfg.set_edge_target(existing_goto_edge, new_block);
}

} // namespace throw_propagation_impl
