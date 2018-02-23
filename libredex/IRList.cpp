/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "IRList.h"

#include <vector>

#include "DexUtil.h"
#include "IRInstruction.h"

MethodItemEntry::MethodItemEntry(const MethodItemEntry& that)
    : type(that.type) {
  switch (type) {
  case MFLOW_TRY:
    tentry = that.tentry;
    break;
  case MFLOW_CATCH:
    centry = that.centry;
    break;
  case MFLOW_OPCODE:
    insn = that.insn;
    break;
  case MFLOW_DEX_OPCODE:
    dex_insn = that.dex_insn;
    break;
  case MFLOW_TARGET:
    target = that.target;
    break;
  case MFLOW_DEBUG:
    new (&dbgop) std::unique_ptr<DexDebugInstruction>(that.dbgop->clone());
    break;
  case MFLOW_POSITION:
    new (&pos) std::unique_ptr<DexPosition>(new DexPosition(*that.pos));
    break;
  case MFLOW_FALLTHROUGH:
    break;
  }
}

MethodItemEntry::~MethodItemEntry() {
  switch (type) {
    case MFLOW_TRY:
      delete tentry;
      break;
    case MFLOW_CATCH:
      delete centry;
      break;
    case MFLOW_TARGET:
      delete target;
      break;
    case MFLOW_DEBUG:
      dbgop.~unique_ptr<DexDebugInstruction>();
      break;
    case MFLOW_POSITION:
      pos.~unique_ptr<DexPosition>();
      break;
    case MFLOW_OPCODE:
    case MFLOW_DEX_OPCODE:
    case MFLOW_FALLTHROUGH:
      /* nothing to delete */
      break;
  }
}

void MethodItemEntry::gather_strings(std::vector<DexString*>& lstring) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_strings(lstring);
    break;
  case MFLOW_DEX_OPCODE:
    dex_insn->gather_strings(lstring);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_strings(lstring);
    break;
  case MFLOW_POSITION:
    // although DexPosition contains strings, these strings don't find their
    // way into the APK
    break;
  case MFLOW_FALLTHROUGH:
    break;
  }
}

void MethodItemEntry::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_methods(lmethod);
    break;
  case MFLOW_DEX_OPCODE:
    dex_insn->gather_methods(lmethod);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_methods(lmethod);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  }
}

void MethodItemEntry::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_fields(lfield);
    break;
  case MFLOW_DEX_OPCODE:
    dex_insn->gather_fields(lfield);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_fields(lfield);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  }
}

void MethodItemEntry::gather_types(std::vector<DexType*>& ltype) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    if (centry->catch_type != nullptr) {
      ltype.push_back(centry->catch_type);
    }
    break;
  case MFLOW_OPCODE:
    insn->gather_types(ltype);
    break;
  case MFLOW_DEX_OPCODE:
    dex_insn->gather_types(ltype);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_types(ltype);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  }
}

opcode::Branchingness MethodItemEntry::branchingness() const {
  switch (type) {
  case MFLOW_OPCODE:
    return opcode::branchingness(insn->opcode());
  case MFLOW_DEX_OPCODE:
    always_assert_log(false, "Not expecting dex instructions here");
    not_reached();
  default:
    return opcode::BRANCH_NONE;
  }
}

void IRList::replace_opcode(IRInstruction* to_delete,
                            std::vector<IRInstruction*> replacements) {
  auto it = m_list.begin();
  for (; it != m_list.end(); it++) {
    if (it->type == MFLOW_OPCODE && it->insn == to_delete) {
      break;
    }
  }
  always_assert_log(it != m_list.end(),
                    "No match found while replacing '%s'",
                    SHOW(to_delete));
  for (auto insn : replacements) {
    insert_before(it, insn);
  }
  remove_opcode(it);
}

void IRList::replace_opcode(IRInstruction* from, IRInstruction* to) {
  always_assert_log(!is_branch(to->opcode()),
                    "You may want replace_branch instead");
  replace_opcode(from, std::vector<IRInstruction*>{to});
}

void IRList::replace_opcode_with_infinite_loop(IRInstruction* from) {
  IRInstruction* to = new IRInstruction(OPCODE_GOTO);
  auto miter = m_list.begin();
  for (; miter != m_list.end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE && mentry->insn == from) {
      if (is_branch(from->opcode())) {
        remove_branch_targets(from);
      }
      mentry->insn = to;
      delete from;
      break;
    }
  }
  always_assert_log(
      miter != m_list.end(),
      "No match found while replacing '%s' with '%s'",
      SHOW(from),
      SHOW(to));
  auto target = new BranchTarget();
  target->type = BRANCH_SIMPLE;
  target->src = &*miter;
  m_list.insert(miter, *(new MethodItemEntry(target)));
}

void IRList::replace_branch(IRInstruction* from, IRInstruction* to) {
  always_assert(is_branch(from->opcode()));
  always_assert(is_branch(to->opcode()));
  for (auto& mentry : m_list) {
    if (mentry.type == MFLOW_OPCODE && mentry.insn == from) {
      mentry.insn = to;
      delete from;
      return;
    }
  }
  always_assert_log(
      false,
      "No match found while replacing '%s' with '%s'",
      SHOW(from),
      SHOW(to));
}

void IRList::insert_after(IRInstruction* position,
                          const std::vector<IRInstruction*>& opcodes) {
  /* The nullptr case handling is strange-ish..., this will not work as expected
   * if a method has a branch target as it's first instruction.
   *
   * To handle this case sanely, we'd need to export a interface based on
   * MEI's probably.
   */
  for (auto const& mei : m_list) {
    if (mei.type == MFLOW_OPCODE &&
        (position == nullptr || mei.insn == position)) {
      auto insert_at = m_list.iterator_to(mei);
      if (position != nullptr) {
        insert_at++;
        if (position->has_move_result_pseudo()) {
          insert_at++;
        }
      }
      for (auto* opcode : opcodes) {
        MethodItemEntry* mentry = new MethodItemEntry(opcode);
        m_list.insert(insert_at, *mentry);
      }
      return;
    }
  }
  always_assert_log(false, "No match found");
}

IRList::iterator IRList::insert_before(
    const IRList::iterator& position, MethodItemEntry& mie) {
  return m_list.insert(position, mie);
}

IRList::iterator IRList::insert_after(
    const IRList::iterator& position, MethodItemEntry& mie) {
  always_assert(position != m_list.end());
  return m_list.insert(std::next(position), mie);
}

void IRList::remove_opcode(const IRList::iterator& it) {
  always_assert(it->type == MFLOW_OPCODE);
  auto insn = it->insn;
  always_assert(!opcode::is_move_result_pseudo(insn->opcode()));
  if (insn->has_move_result_pseudo()) {
    auto move_it = std::next(it);
    always_assert_log(
        move_it->type == MFLOW_OPCODE &&
            opcode::is_move_result_pseudo(move_it->insn->opcode()),
        "No move-result-pseudo found for %s",
        SHOW(insn));
    delete move_it->insn;
    move_it->type = MFLOW_FALLTHROUGH;
    move_it->insn = nullptr;
  }
  if (is_branch(insn->opcode())) {
    remove_branch_targets(insn);
  }
  it->type = MFLOW_FALLTHROUGH;
  it->insn = nullptr;
  delete insn;
}

void IRList::remove_opcode(IRInstruction* insn) {
  for (auto& mei : m_list) {
    if (mei.type == MFLOW_OPCODE && mei.insn == insn) {
      auto it = m_list.iterator_to(mei);
      remove_opcode(it);
      return;
    }
  }
  always_assert_log(false,
                    "No match found while removing '%s' from method",
                    SHOW(insn));
}

/*
 * Param `insn` should be part of a switch...case statement. Find the case
 * block it is contained within and remove it. Then decrement the index of
 * all the other case blocks that are larger than the index of the removed
 * block so that the case numbers don't have any gaps and the switch can
 * still be encoded as a packed-switch opcode.
 *
 * We do the removal by removing the MFLOW_TARGET corresponding to that
 * case label. Its contents are dead code which will be removed by LocalDCE
 * later. (We could do it here too, but LocalDCE already knows how to find
 * block boundaries.)
 */
void IRList::remove_switch_case(IRInstruction* insn) {

  TRACE(MTRANS, 3, "Removing switch case from: %s\n", SHOW(this));
  // Check if we are inside switch method.
  const MethodItemEntry* switch_mei {nullptr};
  for (const auto& mei : InstructionIterable(m_list)) {
    auto op = mei.insn->opcode();
    if (opcode::is_load_param(op)) {
      continue;
    }
    assert_log(is_switch(op), " Method is not a switch");
    switch_mei = &mei;
    break;
  }
  always_assert(switch_mei != nullptr);

  int target_count = 0;
  for (auto& mei : m_list) {
    if (mei.type == MFLOW_TARGET && mei.target->type == BRANCH_MULTI) {
      target_count++;
    }
  }
  assert_log(target_count != 0, " There should be atleast one target");
  if (target_count == 1) {
    auto excpt_str = DexString::make_string("Redex switch Exception");
    std::vector<IRInstruction*> excpt_block;
    create_runtime_exception_block(excpt_str, excpt_block);
    insert_after(insn, excpt_block);
    remove_opcode(insn);
    return;
  }

  // Find the starting MULTI Target point to delete.
  MethodItemEntry* target_mei = nullptr;
  for (auto miter = m_list.begin(); miter != m_list.end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_TARGET) {
      target_mei = mentry;
    }
    // Check if insn belongs to the current block.
    if (mentry->type == MFLOW_OPCODE && mentry->insn == insn) {
      break;
    }
  }
  always_assert_log(target_mei != nullptr,
                    "Could not find target for %s in %s",
                    SHOW(insn),
                    SHOW(this));

  for (const auto& mie : m_list) {
    if (mie.type == MFLOW_TARGET) {
      BranchTarget* bt = mie.target;
      if (bt->src == switch_mei && bt->index > target_mei->target->index) {
        bt->index -= 1;
      }
    }
  }

  target_mei->type = MFLOW_FALLTHROUGH;
  delete target_mei->target;
}

size_t IRList::sum_opcode_sizes() const {
  size_t size {0};
  for (const auto& mie : m_list) {
    if (mie.type == MFLOW_OPCODE) {
      size += mie.insn->size();
    }
  }
  return size;
}

size_t IRList::count_opcodes() const {
  size_t count {0};
  for (const auto& mie : m_list) {
    if (mie.type == MFLOW_OPCODE && !opcode::is_internal(mie.insn->opcode())) {
      ++count;
    }
  }
  return count;
}

/*
 * This method fixes the goto branches when the instruction is removed or
 * replaced by another instruction.
 */
void IRList::remove_branch_targets(IRInstruction *branch_inst) {
  always_assert_log(is_branch(branch_inst->opcode()),
                    "Instruction is not a branch instruction.");
  for (auto miter = m_list.begin(); miter != m_list.end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_TARGET) {
      BranchTarget* bt = mentry->target;
      auto btmei = bt->src;
      if (btmei->insn == branch_inst) {
        mentry->type = MFLOW_FALLTHROUGH;
        delete mentry->target;
      }
    }
  }
}

bool IRList::structural_equals(const IRList& other) {
  auto it1 = m_list.begin();
  auto it2 = other.begin();

  for (; it1 != m_list.end() && it2 != other.end(); it1++, it2++) {
    if (it1->type != it2->type) {
      return false;
    }

    if (it1->type == MFLOW_OPCODE) {
      if (*it1->insn != *it2->insn) {
        return false;
      }
    } else if (it1->type == MFLOW_TARGET) {
      auto branch_target1 = static_cast<BranchTarget*>(it1->target);
      auto branch_target2 = static_cast<BranchTarget*>(it2->target);

      if (branch_target1->index != branch_target2->index) {
        return false;
      }
    }
  }

  return it1 == m_list.end() && it2 == other.end();
}

boost::sub_range<IRList> IRList::get_param_instructions() {
  auto params_end = std::find_if_not(
      m_list.begin(), m_list.end(), [&](const MethodItemEntry& mie) {
        return mie.type == MFLOW_FALLTHROUGH ||
               (mie.type == MFLOW_OPCODE &&
                opcode::is_load_param(mie.insn->opcode()));
      });
  return boost::sub_range<IRList>(m_list.begin(), params_end);
}

void IRList::gather_catch_types(std::vector<DexType*>& ltype) const {
  for (auto& mie : m_list) {
    if (mie.type != MFLOW_CATCH) {
      continue;
    }
    if (mie.centry->catch_type != nullptr) {
      ltype.push_back(mie.centry->catch_type);
    }
  }
}

void IRList::gather_types(std::vector<DexType*>& ltype) const {
  for (auto& mie : m_list) {
    mie.gather_types(ltype);
  }
}

void IRList::gather_strings(std::vector<DexString*>& lstring) const {
  for (auto& mie : m_list) {
    mie.gather_strings(lstring);
  }
}

void IRList::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  for (auto& mie : m_list) {
    mie.gather_fields(lfield);
  }
}

void IRList::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
  for (auto& mie : m_list) {
    mie.gather_methods(lmethod);
  }
}

IRList::iterator IRList::main_block() {
  return std::prev(get_param_instructions().end());
}

IRList::iterator IRList::make_if_block(
    IRList::iterator cur,
    IRInstruction* insn,
    IRList::iterator* false_block) {
  auto if_entry = new MethodItemEntry(insn);
  *false_block = m_list.insert(cur, *if_entry);
  auto bt = new BranchTarget();
  bt->src = if_entry;
  bt->type = BRANCH_SIMPLE;
  auto bentry = new MethodItemEntry(bt);
  return m_list.insert(m_list.end(), *bentry);
}

IRList::iterator IRList::make_if_else_block(
    IRList::iterator cur,
    IRInstruction* insn,
    IRList::iterator* false_block,
    IRList::iterator* true_block) {
  // if block
  auto if_entry = new MethodItemEntry(insn);
  *false_block = m_list.insert(cur, *if_entry);

  // end of else goto
  auto goto_entry = new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
  auto goto_it = m_list.insert(m_list.end(), *goto_entry);

  // main block
  auto main_bt = new BranchTarget();
  main_bt->src = goto_entry;
  main_bt->type = BRANCH_SIMPLE;
  auto mb_entry = new MethodItemEntry(main_bt);
  auto main_block = m_list.insert(goto_it, *mb_entry);

  // else block
  auto else_bt = new BranchTarget();
  else_bt->src = if_entry;
  else_bt->type = BRANCH_SIMPLE;
  auto eb_entry = new MethodItemEntry(else_bt);
  *true_block = m_list.insert(goto_it, *eb_entry);

  return main_block;
}

IRList::iterator IRList::make_switch_block(
    IRList::iterator cur,
    IRInstruction* insn,
    IRList::iterator* default_block,
    std::map<SwitchIndices, IRList::iterator>& cases) {
  auto switch_entry = new MethodItemEntry(insn);
  *default_block = m_list.insert(cur, *switch_entry);
  IRList::iterator main_block = *default_block;
  for (auto case_it = cases.begin(); case_it != cases.end(); ++case_it) {
    auto goto_entry = new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
    auto goto_it = m_list.insert(m_list.end(), *goto_entry);

    auto main_bt = new BranchTarget();
    main_bt->src = goto_entry;
    main_bt->type = BRANCH_SIMPLE;
    auto mb_entry = new MethodItemEntry(main_bt);
    main_block = m_list.insert(++main_block, *mb_entry);

    // Insert all the branch targets jumping from the switch entry.
    // Keep updating the iterator of the case block to point right before the
    // GOTO going back to the end of the switch.
    for (auto idx : case_it->first) {
      auto case_bt = new BranchTarget();
      case_bt->src = switch_entry;
      case_bt->index = idx;
      case_bt->type = BRANCH_MULTI;
      auto eb_entry = new MethodItemEntry(case_bt);
      case_it->second = m_list.insert(goto_it, *eb_entry);
    }
  }
  return main_block;
}

namespace ir_list {

IRInstruction* primary_instruction_of_move_result_pseudo(
    IRList::iterator it) {
  --it;
  always_assert(it->type == MFLOW_OPCODE &&
                it->insn->has_move_result_pseudo());
  return it->insn;
}

IRInstruction* move_result_pseudo_of(IRList::iterator it) {
  ++it;
  always_assert(it->type == MFLOW_OPCODE &&
                opcode::is_move_result_pseudo(it->insn->opcode()));
  return it->insn;
}

} // namespace ir_list
