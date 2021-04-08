/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObjectInlinePlugin.h"

#include "CFGInliner.h"
#include "CFGMutation.h"
#include "IROpcode.h"
#include "MethodUtil.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include <unordered_set>

using namespace cfg;
using namespace object_inliner_plugin;

ObjectInlinePlugin::ObjectInlinePlugin(
    const FieldSetMap& field_sets,
    const std::unordered_map<DexFieldRef*, DexFieldRef*>& field_swaps,
    const std::vector<reg_t>& srcs,
    boost::optional<reg_t> value_register,
    boost::optional<reg_t> caller_this,
    DexType* callee_type)
    : m_initial_field_sets(field_sets),
      m_field_swaps(field_swaps),
      m_srcs(srcs),
      m_value_reg(value_register),
      m_caller_this_reg(caller_this) {}

boost::optional<const std::vector<reg_t>&> ObjectInlinePlugin::inline_srcs() {
  return m_srcs;
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
 * Does not use callee.
 */
bool ObjectInlinePlugin::update_before_reg_remap(ControlFlowGraph* caller,
                                                 ControlFlowGraph* callee) {
  // Assumes only updating for one object being inlined.
  bool allocated = false;
  cfg::CFGMutation m(*caller);

  // Allocate registers for all the fields sets that are not field swap.
  // These fields will be removed and the new rgisters will take their place.
  for (auto& it : m_initial_field_sets) {
    auto* field = it.first;
    if (m_field_swaps.count(field)) {
      continue;
    }
    auto final_field = m_set_field_sets.find(field);
    if (final_field == m_set_field_sets.end()) {
      allocated = true;
      IRInstruction* set_default = nullptr;
      reg_t assign_reg;

      if (type::is_wide_type(field->get_type())) {
        assign_reg = caller->allocate_wide_temp();
        set_default = new IRInstruction(OPCODE_CONST_WIDE);
        set_default->set_literal(0);
        set_default->set_dest(assign_reg);
      } else {
        assign_reg = caller->allocate_temp();
        set_default = new IRInstruction(OPCODE_CONST);
        set_default->set_literal(0);
        set_default->set_dest(assign_reg);
      }
      auto st = caller->entry_block();
      auto field_set_data = it.second;
      if (st->get_first_non_param_loading_insn() != st->end()) {
        m.insert_before(
            caller->find_insn(st->get_first_non_param_loading_insn()->insn),
            {set_default});
      } else {
        m.insert_after(caller->find_insn(st->get_last_insn()->insn),
                       {set_default});
      }
      m_set_field_sets[field] = {{{assign_reg, {}}}};
    }
  }

  auto iterable = cfg::InstructionIterable(*caller);
  for (auto insn_it = iterable.begin(); insn_it != iterable.end(); ++insn_it) {
    IRInstruction* insn = insn_it->insn;
    auto opcode = insn->opcode();

    if (opcode::is_an_iput(opcode)) {
      auto current_reg = insn->srcs()[0];
      auto field = insn->get_field();
      auto field_set_to_move = m_initial_field_sets.find(field);
      auto final_field = m_set_field_sets.find(field);
      if (field_set_to_move == m_initial_field_sets.end()) {
        continue;
      }
      auto field_set_data = field_set_to_move->second;
      auto reg = field_set_data.find(current_reg);
      if (reg == field_set_data.end()) {
        // can't be the instruction we want to replace
        continue;
      }
      if (reg->second.count(insn) == 0) {
        // can't be the instruction we want to replace
        continue;
      }
      auto move = new IRInstruction(opcode::iput_to_move(opcode));
      move->set_src(0, current_reg);
      assert(final_field != m_set_field_sets.end());
      // There will be only one, so the loop is just to pull out the first
      assert(final_field->second.size() == 1);
      const auto& assign_reg = *final_field->second.begin();
      move->set_dest(assign_reg.first);
      m.replace(insn_it, {move});
      continue;
    }
  }
  m.flush();
  return allocated;
}

/*
 * Convert igets on 'this' in callee into moves from registers stored in
 * m_set_field_sets.
 * If a field is extracted but was not set, introduce a const 0
 * instruction as a default (likely null) value.
 */
bool ObjectInlinePlugin::update_after_reg_remap(ControlFlowGraph*,
                                                ControlFlowGraph* callee) {

  // After remap, this reg has been moved and
  // load params have been changed to moves
  IRInstruction* original_load_this = callee->entry_block()->begin()->insn;
  reg_t callee_this = original_load_this->dest();
  std::unordered_set<DexFieldRef*> used_fields;
  std::unordered_set<reg_t> this_refs = {callee_this};

  cfg::CFGMutation m(*callee);
  auto iterable = cfg::InstructionIterable(*callee);
  for (auto insn_it = iterable.begin(); insn_it != iterable.end(); ++insn_it) {
    IRInstruction* insn = insn_it->insn;
    auto opcode = insn->opcode();
    if (opcode == OPCODE_INVOKE_DIRECT && method::is_init(insn->get_method()) &&
        this_refs.count(insn->src(0))) {
      m.remove(insn_it);
    } else if (opcode::is_an_iput(opcode)) {
      auto field = insn->get_field();
      bool is_self_call = this_refs.count(insn->src(1)) != 0;
      if (is_self_call) {
        auto no_field_needed = m_set_field_sets.find(field);
        auto swap_field = m_field_swaps.find(field);
        if (swap_field != m_field_swaps.end()) {
          assert(m_caller_this_reg);
          insn->set_field(swap_field->second);
          insn->set_src(1, m_caller_this_reg.value());
          used_fields.emplace(swap_field->first);
          continue;
        }

        assert(no_field_needed != m_set_field_sets.end());
        no_field_needed = m_set_field_sets.find(field);
        auto move = new IRInstruction(opcode::iput_to_move(opcode));
        assert(no_field_needed->second.size() == 1);
        // Extract the solo reg, and set as src.
        move->set_dest(no_field_needed->second.begin()->first);
        move->set_src(0, insn->src(0));
        used_fields.emplace(field);
        m.replace(insn_it, {move});
      }
    } else if (opcode::is_an_iget(opcode)) {
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
          IRInstruction* set_default = nullptr;
          auto move_result = callee->move_result_of(callee->find_insn(insn));
          if (move_result->insn->dest_is_wide()) {
            set_default = new IRInstruction(OPCODE_CONST_WIDE);
            set_default->set_literal(0);
          } else {
            set_default = new IRInstruction(OPCODE_CONST);
            set_default->set_literal(0);
          }
          set_default->set_dest(move_result->insn->dest());
          m.remove(move_result);
          m.replace(insn_it, {set_default});
        } else {
          auto move = new IRInstruction(opcode::iget_to_move(opcode));
          assert(no_field_needed->second.size() == 1);
          // Extract the solo reg, and set as src.
          auto move_result = callee->move_result_of(callee->find_insn(insn));
          move->set_src(0, no_field_needed->second.begin()->first);
          move->set_dest(move_result->insn->dest());
          m.remove(move_result);
          used_fields.emplace(field);
          m.replace(insn_it, {move});
        }
      }
    }
    if (insn != original_load_this && insn->has_dest()) {
      if (opcode::is_a_move(opcode) && this_refs.count(insn->src(0))) {
        this_refs.insert(insn->dest());
      } else {
        this_refs.erase(insn->dest());
      }
    }
  }
  m.flush();
  // Registers were changed. A full recompute is needed.
  return true;
}
