/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUnusedArgs.h"

#include <unordered_map>
#include <vector>

#include "CallGraph.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Liveness.h"
#include "Walkers.h"

// NOTE:
//  TODO <anwangster> (*) indicates goal for next diff

namespace remove_unused_args {

constexpr const char* METRIC_ARGS_REMOVED = "callsite_args_removed";

void RemoveArgs::run() {
  m_num_regs_removed = 0;
  update_meths_with_unused_args();
  update_callsites();
}

/**
 * Returns a vector of live argument registers in the given method's code.
 * In the IR, invoke instructions do not specify both regs for wide args, so
 * it's ok for us to identify wide arg reg pairs with just the first reg.
 */
std::vector<uint16_t> RemoveArgs::compute_live_arg_regs(IRCode* code) {
  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain(code->get_registers_size()));
  auto entry_block = cfg.entry_block();

  std::vector<uint16_t> live_arg_regs;

  auto live_vars = fixpoint_iter.get_live_out_vars_at(entry_block);
  for (auto it = entry_block->rbegin(); it != entry_block->rend(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    auto insn = it->insn;
    if (opcode::is_load_param(insn->opcode()) &&
        live_vars.contains(insn->dest())) {
      // insn->dest() is a live arg reg
      live_arg_regs.emplace_back(insn->dest());
    }
    fixpoint_iter.analyze_instruction(insn, &live_vars);
  }

  std::sort(live_arg_regs.begin(), live_arg_regs.end());
  return live_arg_regs;
}

/**
 * For methods that have unused arguments, record live argument registers
 * in m_live_regs_map.
 */
void RemoveArgs::update_meths_with_unused_args() {
  walk::methods(m_scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code == nullptr) {
      return;
    }

    auto proto = method->get_proto();
    auto num_args = proto->get_args()->size();
    if (num_args == 0) {
      // Nothing to do if the method doesn't have args to remove
      return;
    }

    auto live_arg_regs = compute_live_arg_regs(code);
    if (live_arg_regs.size() == num_args) {
      // No removable arguments, so don't update method arg lists
      return;
    }
    m_live_regs_map.emplace(method, live_arg_regs);

    // Record the original base arg reg to properly update method's arg list
    auto first_insn = InstructionIterable(code).begin()->insn;
    always_assert_log(
        opcode::is_load_param(first_insn->opcode()),
        "First instruction of method with args should be a load param\n");
    uint16_t arg_base_reg = first_insn->dest();

    // TODO <anwangster> (*)
    //  update method signature
    //    use std::dequeue<DexType*> to build updated arguments
    //    try to get method m for updated type, name, proto
    //    if m is NOT null, we have a collision.
    //    if m is a ctor and collides, remove its entry from the map and return
    //    on collision do some name mangling (use method->change(, true))
  });
}

/**
 * Removes dead arguments from the given invoke instr if applicable
 */
void RemoveArgs::update_callsite(IRInstruction* instr) {
  auto method_ref = instr->get_method();
  if (!method_ref->is_def()) {
    // TODO <anwangster> deal with virtual methods separately
    return;
  };
  DexMethod* method = resolve_method(method_ref, opcode_to_search(instr));

  auto kv_pair = m_live_regs_map.find(method);
  if (kv_pair == m_live_regs_map.end()) {
    // No removable arguments, so do nothing
    return;
  }

  // TODO <anwangster> (*)
  // we must update the invoke instructions when the method sig changes
  // update invoke instruction arguments
  //    instr->set_arg_word_count()
  //    copy updated srcs over to instr's srcs, use instr->set_src()
  // update m_num_regs_removed
}

/**
 * Removes unused arguments at callsites.
 * Returns the number of arguments removed.
 */
void RemoveArgs::update_callsites() {
  // walk through all methods to look for and edit callsites
  walk::methods(m_scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code == nullptr) {
      return;
    }

    for (const auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      auto opcode = insn->opcode();

      if (opcode == OPCODE_INVOKE_DIRECT || opcode == OPCODE_INVOKE_STATIC) {
        update_callsite(insn);
      } else if (opcode == OPCODE_INVOKE_VIRTUAL) {
        // TODO <anwangster>
        //   maybe coalesce with above branch
      }
    }
  });
}

void RemoveUnusedArgsPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& cfg,
                                    PassManager& mgr) {
  auto scope = build_class_scope(stores);
  RemoveArgs rm_args(scope);
  rm_args.run();
  size_t num_regs_removed = rm_args.get_num_regs_removed();

  TRACE(ARGS,
        1,
        "ARGS :| Removed %d redundant callsite argument registers\n",
        num_regs_removed);

  mgr.set_metric(METRIC_ARGS_REMOVED, num_regs_removed);
}

static RemoveUnusedArgsPass s_pass;

} // namespace remove_unused_args
