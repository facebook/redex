/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObjectInlinePlugin.h"

#include "CFGInliner.h"
#include "ClassInitCounter.h"
#include "IROpcode.h"

using namespace cic;
using namespace cfg;

ObjectInlinePlugin::ObjectInlinePlugin(
    const FieldSetMap& field_sets,
    const std::map<DexFieldRef*, DexFieldRef*, dexfields_comparator>&
        field_swaps,
    const std::vector<reg_t>& srcs,
    reg_t value_register,
    boost::optional<reg_t> caller_this,
    reg_t callee_this,
    DexType* callee_type)
    : m_initial_field_sets(field_sets),
      m_field_swaps(field_swaps),
      m_srcs(srcs),
      m_value_reg(value_register),
      m_caller_this_reg(caller_this),
      m_callee_this_reg(callee_this),
      m_callee_class(callee_type) {}

const boost::optional<std::reference_wrapper<std::vector<reg_t>>>
ObjectInlinePlugin::inline_srcs() {
  return std::reference_wrapper<std::vector<reg_t>>(m_srcs);
}

boost::optional<reg_t> ObjectInlinePlugin::reg_for_return() {
  return m_value_reg;
}

bool ObjectInlinePlugin::inline_after() { return false; }

bool ObjectInlinePlugin::remove_inline_site() { return false; }

/*
 * Convert field iputs in caller to moves when object is inlined, according to
 * the analysis data in m_initial_field_sets
 * Save what register a field value is being moved into into m_set_field_sets
 * Can increase the number of registers in caller
 * Does not use callee.
 */
void ObjectInlinePlugin::update_before_reg_remap(ControlFlowGraph* caller,
                                                 ControlFlowGraph* callee) {
  // Assumes only updating for one object being inlined.
  for (auto block : caller->blocks()) {
    for (auto& mie : ir_list::InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      auto opcode = insn->opcode();

      if (is_iput(opcode)) {
        auto current_reg = insn->srcs()[0];
        auto field = insn->get_field();
        auto field_set_to_move = m_initial_field_sets.find(field);
        auto final_field = m_set_field_sets.find(field);
        if (field_set_to_move == m_initial_field_sets.end()) {
          continue;
        }
        auto field_set_data = field_set_to_move->second;
        auto reg = field_set_data.regs.find(current_reg);
        if (reg == field_set_data.regs.end()) {
          // can't be the instruction we want to replace
          continue;
        }
        if (reg->second.count(insn) == 0) {
          // can't be the instruction we want to replace
          continue;
        }
        auto move = new IRInstruction(opcode::iput_to_move(opcode));
        move->set_src(0, current_reg);
        if (final_field == m_set_field_sets.end()) {
          reg_t assign_reg = caller->allocate_temp();
          m_set_field_sets[field] = {
              {{assign_reg, {}}}, field_set_data.set, cic::OneReg};
          move->set_dest(assign_reg);
        } else {
          // There will be only one, so the loop is just to pull out the first
          for (auto assign_reg : final_field->second.regs) {
            move->set_dest(assign_reg.first);
            break;
          }
        }
        delete mie.insn;
        mie.insn = move;
        continue;
      }
    }
  }
}

/*
 * Convert igets on 'this' in callee into moves from registers stored in
 * m_set_field_sets.
 * If a field is extracted but was not set, introduce a const 0
 * instruction as a default (likely null) value.
 * Store unaccessed fields in m_unaccessed_field_sets for others to
 * handle
 */
bool ObjectInlinePlugin::update_after_reg_remap(ControlFlowGraph*,
                                                ControlFlowGraph* callee) {

  // After remap, this reg has been moved and
  // load params have been changed to moves
  IRInstruction* original_load_this = callee->entry_block()->begin()->insn;
  reg_t callee_this = original_load_this->dest();
  std::set<DexFieldRef*, dexfields_comparator> used_fields;
  std::set<reg_t> this_refs = {callee_this};

  for (auto block : callee->blocks()) {
    IRInstruction* awaiting_dest_instr = nullptr;
    ir_list::InstructionIterable iterator(block);
    std::vector<ir_list::InstructionIterator> to_remove = {};
    for (auto it = iterator.begin(); it != iterator.end(); it++) {
      IRInstruction* insn = it->insn;
      auto opcode = insn->opcode();
      if (is_iget(opcode)) {
        auto field = insn->get_field();
        bool is_self_call = this_refs.count(insn->src(0)) != 0;
        if (is_self_call) {
          auto no_field_needed = m_set_field_sets.find(field);
          auto swap_field = m_field_swaps.find(field);
          TRACE(CFG,
                4,
                "ObjectPlugin update callee, looking at field %s",
                SHOW(insn));

          if (swap_field != m_field_swaps.end()) {
            assert(m_caller_this_reg);
            insn->set_field(swap_field->second);
            insn->set_src(0, m_caller_this_reg.value());
            used_fields.emplace(swap_field->first);
            continue;
          }
          if (no_field_needed == m_set_field_sets.end()) {
            auto set_default = new IRInstruction(OPCODE_CONST);
            set_default->set_literal(0);
            awaiting_dest_instr = set_default;
          } else {
            auto move = new IRInstruction(opcode::iget_to_move(opcode));
            assert(no_field_needed->second.regs.size() == 1);
            // Extract the solo reg, and set as src.
            move->set_src(0, no_field_needed->second.regs.begin()->first);
            used_fields.emplace(field);
            awaiting_dest_instr = move;
          }
          delete it->insn;
          it->insn = awaiting_dest_instr;
          continue;
        }
      } else if (is_move(opcode)) {
        // track this references to aid in redirection
        if (this_refs.count(insn->dest()) != 0 &&
            this_refs.count(insn->src(0)) != 0) {
          // No change move
        } else if (insn != original_load_this &&
                   this_refs.count(insn->dest()) != 0 &&
                   this_refs.count(insn->src(0)) == 0) {
          this_refs.erase(insn->dest());
        } else if (this_refs.count(insn->src(0)) != 0) {
          this_refs.insert(insn->dest());
        }
      } else if (opcode::is_move_result_any(opcode)) {
        if (awaiting_dest_instr != nullptr) {
          awaiting_dest_instr->set_dest(insn->dest());
          to_remove.emplace_back(it);
        }
      }
      awaiting_dest_instr = nullptr;
    }
    for (auto it : to_remove) {
      block->remove_insn(it);
    }
  }
  for (auto fs : m_set_field_sets) {
    if (used_fields.count(fs.first) == 0) {
      m_unaccessed_field_sets.insert(fs);
    }
  }
  // Registers were changed. A full recompute is needed.
  return true;
}
