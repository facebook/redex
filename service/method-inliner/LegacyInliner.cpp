/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LegacyInliner.h"

#include "DexPosition.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "Show.h"
#include "Trace.h"
#include "Transform.h"

namespace {

using RegMap = transform::RegMap;

/*
 * Expands the caller register file by the size of the callee register file,
 * and allocates the high registers to the callee. E.g. if we have a caller
 * with registers_size of M and a callee with registers_size N, this function
 * will resize the caller's register file to M + N and map register k in the
 * callee to M + k in the caller. It also inserts move instructions to map the
 * callee arguments to the newly allocated registers.
 */
std::unique_ptr<RegMap> gen_callee_reg_map(IRCode* caller_code,
                                           const IRCode* callee_code,
                                           const IRList::iterator& invoke_it) {
  auto callee_reg_start = caller_code->get_registers_size();
  auto insn = invoke_it->insn;
  auto reg_map = std::make_unique<RegMap>();

  // generate the callee register map
  for (reg_t i = 0; i < callee_code->get_registers_size(); ++i) {
    reg_map->emplace(i, callee_reg_start + i);
  }

  // generate and insert the move instructions
  auto param_insns = InstructionIterable(callee_code->get_param_instructions());
  auto param_it = param_insns.begin();
  auto param_end = param_insns.end();
  for (size_t i = 0; i < insn->srcs_size(); ++i, ++param_it) {
    always_assert(param_it != param_end);
    auto mov = (new IRInstruction(
                    opcode::load_param_to_move(param_it->insn->opcode())))
                   ->set_src(0, insn->src(i))
                   ->set_dest(callee_reg_start + param_it->insn->dest());
    caller_code->insert_before(invoke_it, mov);
  }
  caller_code->set_registers_size(callee_reg_start +
                                  callee_code->get_registers_size());
  return reg_map;
}

/**
 * Create a move instruction given a return instruction in a callee and
 * a move-result instruction in a caller.
 */
IRInstruction* move_result(IRInstruction* res, IRInstruction* move_res) {
  auto move_opcode = opcode::return_to_move(res->opcode());
  IRInstruction* move = (new IRInstruction(move_opcode))
                            ->set_dest(move_res->dest())
                            ->set_src(0, res->src(0));
  return move;
}

} // namespace

/*
 * For splicing a callee's IRList into a caller.
 */
class MethodSplicer {
  IRCode* m_mtcaller;
  MethodItemEntryCloner m_mie_cloner;
  const RegMap& m_callee_reg_map;
  DexPosition* m_invoke_position;
  MethodItemEntry* m_active_catch;
  std::unordered_set<reg_t> m_valid_dbg_regs;

 public:
  MethodSplicer(IRCode* mtcaller,
                const RegMap& callee_reg_map,
                DexPosition* invoke_position,
                MethodItemEntry* active_catch)
      : m_mtcaller(mtcaller),
        m_callee_reg_map(callee_reg_map),
        m_invoke_position(invoke_position),
        m_active_catch(active_catch) {}

  MethodItemEntry* clone(MethodItemEntry* mie) {
    auto result = m_mie_cloner.clone(mie);
    return result;
  }

  void operator()(const IRList::iterator& insert_pos,
                  const IRList::iterator& fcallee_start,
                  const IRList::iterator& fcallee_end) {
    std::vector<DexPosition*> positions_to_fix;
    for (auto it = fcallee_start; it != fcallee_end; ++it) {
      if (should_skip_debug(&*it)) {
        continue;
      }
      if (it->type == MFLOW_OPCODE &&
          opcode::is_a_load_param(it->insn->opcode())) {
        continue;
      }
      auto mie = clone(&*it);
      transform::remap_registers(*mie, m_callee_reg_map);
      if (mie->type == MFLOW_TRY && m_active_catch != nullptr) {
        auto tentry = mie->tentry;
        // try ranges cannot be nested, so we flatten them here
        switch (tentry->type) {
        case TRY_START:
          m_mtcaller->insert_before(
              insert_pos, *(new MethodItemEntry(TRY_END, m_active_catch)));
          m_mtcaller->insert_before(insert_pos, *mie);
          break;
        case TRY_END:
          m_mtcaller->insert_before(insert_pos, *mie);
          m_mtcaller->insert_before(
              insert_pos, *(new MethodItemEntry(TRY_START, m_active_catch)));
          break;
        }
      } else {
        if (mie->type == MFLOW_POSITION && mie->pos->parent == nullptr) {
          mie->pos->parent = m_invoke_position;
        }
        // if a handler list does not terminate in a catch-all, have it point
        // to the parent's active catch handler. TODO: Make this more precise
        // by checking if the parent catch type is a subtype of the callee's.
        if (mie->type == MFLOW_CATCH && mie->centry->next == nullptr &&
            mie->centry->catch_type != nullptr) {
          mie->centry->next = m_active_catch;
        }
        m_mtcaller->insert_before(insert_pos, *mie);
      }
    }
  }

  void fix_parent_positions() {
    m_mie_cloner.fix_parent_positions(m_invoke_position);
  }

 private:
  /* We need to skip two cases:
   * Duplicate DBG_SET_PROLOGUE_END
   * Uninitialized parameters
   *
   * The parameter names are part of the debug info for the method.
   * The technically correct solution would be to make a start
   * local for each of them.  However, that would also imply another
   * end local after the tail to correctly set what the register
   * is at the end.  This would bloat the debug info parameters for
   * a corner case.
   *
   * Instead, we just delete locals lifetime information for parameters.
   * This is an exceedingly rare case triggered by goofy code that
   * reuses parameters as locals.
   */
  bool should_skip_debug(const MethodItemEntry* mei) {
    if (mei->type != MFLOW_DEBUG) {
      return false;
    }
    switch (mei->dbgop->opcode()) {
    case DBG_SET_PROLOGUE_END:
      return true;
    case DBG_START_LOCAL:
    case DBG_START_LOCAL_EXTENDED: {
      auto reg = mei->dbgop->uvalue();
      m_valid_dbg_regs.insert(reg);
      return false;
    }
    case DBG_END_LOCAL:
    case DBG_RESTART_LOCAL: {
      auto reg = mei->dbgop->uvalue();
      if (m_valid_dbg_regs.find(reg) == m_valid_dbg_regs.end()) {
        return true;
      }
    }
      FALLTHROUGH_INTENDED;
    default:
      return false;
    }
  }
};

namespace legacy_inliner {

DexPosition* last_position_before(const IRList::const_iterator& it,
                                  const IRCode* code) {
  // we need to decrement the reverse iterator because it gets constructed
  // as pointing to the element preceding pos
  auto position_it = std::prev(IRList::const_reverse_iterator(it));
  const auto& rend = code->rend();
  while (++position_it != rend && position_it->type != MFLOW_POSITION)
    ;
  return position_it == rend ? nullptr : position_it->pos.get();
}

void inline_method(DexMethod* caller,
                   IRCode* callee_code,
                   const IRList::iterator& pos) {
  change_visibility(callee_code, caller->get_class(), caller);
  inline_method_unsafe(caller, caller->get_code(), callee_code, pos);
}

void inline_method_unsafe(const DexMethod* caller_method,
                          IRCode* caller_code,
                          IRCode* callee_code,
                          const IRList::iterator& pos) {
  TRACE(INL, 5, "caller code:\n%s", SHOW(caller_code));
  TRACE(INL, 5, "callee code:\n%s", SHOW(callee_code));

  if (caller_code->get_debug_item() == nullptr && caller_method != nullptr) {
    // Create an empty item so that debug info of inlinee does not get lost.
    caller_code->set_debug_item(std::make_unique<DexDebugItem>());
    // Create a fake position.
    auto it = caller_code->main_block();
    if (it != caller_code->end()) {
      caller_code->insert_after(
          caller_code->main_block(),
          *(new MethodItemEntry(
              DexPosition::make_synthetic_entry_position(caller_method))));
    } else {
      caller_code->push_back(*(new MethodItemEntry(
          DexPosition::make_synthetic_entry_position(caller_method))));
    }
  }

  auto callee_reg_map = gen_callee_reg_map(caller_code, callee_code, pos);

  // find the move-result after the invoke, if any. Must be the first
  // instruction after the invoke
  auto move_res = pos;
  while (move_res++ != caller_code->end() && move_res->type != MFLOW_OPCODE)
    ;
  if (!opcode::is_a_move_result(move_res->insn->opcode())) {
    move_res = caller_code->end();
  }

  // find the last position entry before the invoke.
  const auto invoke_position = last_position_before(pos, caller_code);
  if (invoke_position) {
    TRACE(INL, 3, "Inlining call at %s:%d", invoke_position->file->c_str(),
          invoke_position->line);
  }

  // check if we are in a try block
  auto caller_catch = transform::find_active_catch(caller_code, pos);

  const auto& ret_it = std::find_if(
      callee_code->begin(), callee_code->end(), [](const MethodItemEntry& mei) {
        return mei.type == MFLOW_OPCODE &&
               opcode::is_a_return(mei.insn->opcode());
      });

  auto splice = MethodSplicer(caller_code, *callee_reg_map, invoke_position,
                              caller_catch);
  // Copy the callee up to the return. Everything else we push at the end
  // of the caller
  splice(pos, callee_code->begin(), ret_it);

  // try items can span across a return opcode
  auto callee_catch =
      splice.clone(transform::find_active_catch(callee_code, ret_it));
  if (callee_catch != nullptr) {
    caller_code->insert_before(pos,
                               *(new MethodItemEntry(TRY_END, callee_catch)));
    if (caller_catch != nullptr) {
      caller_code->insert_before(
          pos, *(new MethodItemEntry(TRY_START, caller_catch)));
    }
  }

  if (move_res != caller_code->end() && ret_it != callee_code->end()) {
    auto ret_insn = std::make_unique<IRInstruction>(*ret_it->insn);
    transform::remap_registers(ret_insn.get(), *callee_reg_map);
    IRInstruction* move = move_result(ret_insn.get(), move_res->insn);
    auto move_mei = new MethodItemEntry(move);
    caller_code->insert_before(pos, *move_mei);
  }
  // ensure that the caller's code after the inlined method retain their
  // original position
  if (invoke_position) {
    caller_code->insert_before(
        pos,
        *(new MethodItemEntry(
            std::make_unique<DexPosition>(*invoke_position))));
  }

  // remove invoke
  caller_code->erase_and_dispose(pos);
  // remove move_result
  if (move_res != caller_code->end()) {
    caller_code->erase_and_dispose(move_res);
  }

  if (ret_it != callee_code->end()) {
    if (callee_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_START, callee_catch)));
    } else if (caller_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_START, caller_catch)));
    }

    if (std::next(ret_it) != callee_code->end()) {
      const auto return_position = last_position_before(ret_it, callee_code);
      if (return_position) {
        // If there are any opcodes between the callee's return and its next
        // position, we need to re-mark them with the correct line number,
        // otherwise they would inherit the line number from the end of the
        // caller.
        auto new_pos = std::make_unique<DexPosition>(*return_position);
        // We want its parent to be the same parent as other inlined code.
        new_pos->parent = invoke_position;
        caller_code->push_back(*(new MethodItemEntry(std::move(new_pos))));
      }
    }

    // Copy the opcodes in the callee after the return and put them at the end
    // of the caller.
    splice(caller_code->end(), std::next(ret_it), callee_code->end());
    if (caller_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_END, caller_catch)));
    }
  }
  splice.fix_parent_positions();
  TRACE(INL, 5, "post-inline caller code:\n%s", SHOW(caller_code));
}

} // namespace legacy_inliner
