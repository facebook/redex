/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RemoveBuildersHelper.h"

#include <boost/dynamic_bitset.hpp>
#include <boost/regex.hpp>

#include "ControlFlow.h"
#include "Dataflow.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "Transform.h"

namespace {

void fields_mapping(const IRInstruction* insn,
                    FieldsRegs* fregs,
                    DexClass* builder,
                    bool is_setter) {
  always_assert(insn != nullptr);
  always_assert(fregs != nullptr);
  always_assert(builder != nullptr);

  bool new_builder = false;
  if (insn->opcode() == OPCODE_NEW_INSTANCE) {
    if (insn->get_type() == builder->get_type()) {
      new_builder = true;
    }
  }

  // Set fields to UNDEFINED if new builder instance.
  if (new_builder) {
    for (auto& pair : fregs->field_to_reg) {
      fregs->field_to_reg[pair.first] = FieldOrRegStatus::UNDEFINED;
      fregs->field_to_iput_insns[pair.first].clear();
    }
  }

  // Check if the register that used to hold the field's value is overwritten.
  if (insn->dests_size()) {
    const int current_dest = insn->dest();

    for (const auto& pair : fregs->field_to_reg) {
      if (pair.second == current_dest) {
        fregs->field_to_reg[pair.first] = FieldOrRegStatus::OVERWRITTEN;
      }

      if (insn->dest_is_wide()) {
        if (pair.second == current_dest + 1) {
          fregs->field_to_reg[pair.first] = FieldOrRegStatus::OVERWRITTEN;
        }
      }
    }
  }

  if ((is_setter && is_iput(insn->opcode())) ||
      (!is_setter && is_iget(insn->opcode()))) {
    auto field = insn->get_field();

    if (field->get_class() == builder->get_type()) {
      uint16_t current = is_setter ? insn->src(0) : insn->dest();
      fregs->field_to_reg[field] = current;
      if (is_setter) {
        fregs->field_to_iput_insns[field].clear();
        fregs->field_to_iput_insns[field].emplace(insn);
      }
    }
  }
}

/**
 * Returns for every instruction, field value:
 * - a register: representing the register that stores the field's value
 * - UNDEFINED: not defined yet.
 * - DIFFERENT: no unique register.
 * - OVERWRITTEN: register no longer holds the value.
 */
std::unique_ptr<std::unordered_map<IRInstruction*, FieldsRegs>> fields_setters(
    const std::vector<Block*>& blocks, DexClass* builder) {

  std::function<void(const IRInstruction*, FieldsRegs*)> trans = [&](
      const IRInstruction* insn, FieldsRegs* fregs) {
    fields_mapping(insn, fregs, builder, true);
  };

  return forwards_dataflow(blocks, FieldsRegs(builder), trans);
}

bool enlarge_register_frame(DexMethod* method, uint16_t extra_regs) {

  always_assert(method != nullptr);

  auto oldregs = method->get_code()->get_registers_size();
  auto newregs = oldregs + extra_regs;

  if (newregs > 16) {
    return false;
  }
  IRCode::enlarge_regs(method, newregs);
  return true;
}

DexOpcode get_move_opcode(const IRInstruction* insn) {
  always_assert(insn != nullptr);
  always_assert(is_iget(insn->opcode()));

  if (insn->opcode() == OPCODE_IGET_WIDE) {
    return OPCODE_MOVE_WIDE;
  } else if (insn->opcode() == OPCODE_IGET_OBJECT) {
    return OPCODE_MOVE_OBJECT;
  }

  return OPCODE_MOVE;
}

/**
 * Adds an instruction that initializes a new register with null.
 */
void add_null_instr(IRCode* code, uint16_t reg) {
  always_assert(code != nullptr);

  IRInstruction* insn = new IRInstruction(OPCODE_CONST_4);

  insn->set_dest(reg);
  insn->set_literal(0);

  std::vector<IRInstruction*> insns;
  insns.push_back(insn);

  // Adds the instruction at the beginning, since it might be
  // used in various places later.
  code->insert_after(nullptr, insns);
}

IRInstruction* construct_move_instr(uint16_t dest_reg,
                                    uint16_t src_reg,
                                    DexOpcode move_opcode) {
  IRInstruction* insn = new IRInstruction(move_opcode);
  insn->set_dest(dest_reg);
  insn->set_src(0, src_reg);
  return insn;
}

IRInstruction* construct_null_instr(uint16_t reg) {
  IRInstruction* insn = new IRInstruction(OPCODE_CONST_4);
  insn->set_dest(reg);
  insn->set_literal(0);
  return insn;
}

/**
 * Adds instructions that initializes registers with null.
 */
void null_initializations(IRCode* code,
                          const std::vector<uint16_t>& null_regs) {
  always_assert(code != nullptr);

  std::vector<IRInstruction*> insns;
  for (uint16_t null_reg : null_regs) {
    insns.push_back(construct_null_instr(null_reg));
  }

  // Adds the instructions at the beginnings.
  code->insert_after(nullptr, insns);
}

void add_instr(IRCode* code,
               const IRInstruction* position,
               IRInstruction* insn) {
  always_assert(code != nullptr);
  always_assert(position != nullptr);
  always_assert(insn != nullptr);

  std::vector<IRInstruction*> insns;
  insns.push_back(insn);

  code->insert_after(const_cast<IRInstruction*>(position), insns);
}

using MoveList = std::unordered_map<const IRInstruction*, IRInstruction*>;

/**
 * Updates parameter registers to account for the extra registers.
 */
void update_reg_params(const std::unordered_set<IRInstruction*>& update_list,
                       uint16_t non_input_reg_size,
                       uint16_t extra_regs,
                       MoveList& move_list) {

  for (const auto& update : update_list) {
    IRInstruction* new_insn = move_list[update];
    new_insn->set_src(0, new_insn->src(0) + extra_regs);
  }

  for (const auto& move_elem : move_list) {
    const IRInstruction* old_insn = move_elem.first;
    IRInstruction* new_insn = move_elem.second;

    if (is_iput(old_insn->opcode())) {
      if (old_insn->src(0) >= non_input_reg_size) {
        new_insn->set_src(0, new_insn->src(0) + extra_regs);
      }
    } else if (is_iget(old_insn->opcode())) {
      if (old_insn->dest() >= non_input_reg_size) {
        new_insn->set_dest(new_insn->dest() + extra_regs);
      }
    }
  }
}

void method_updates(DexMethod* method,
                    const std::vector<IRInstruction*>& deletes,
                    const MoveList& move_list) {
  always_assert(method != nullptr);

  auto code = method->get_code();

  // This will basically replace an iput / iget instruction
  // with a move (giving the instruction will be removed later).
  //
  // Example:
  //  iput v0, object // field -> move new_reg, v0
  //  iget v0, object // field -> move v0, new_reg
  for (const auto& move_elem : move_list) {
    const IRInstruction* position = move_elem.first;
    IRInstruction* insn = move_elem.second;
    add_instr(code, position, insn);
  }

  for (const auto& insn : deletes) {
    code->remove_opcode(insn);
  }
}

} // namespace

///////////////////////////////////////////////

void TaintedRegs::meet(const TaintedRegs& that) { m_reg_set |= that.m_reg_set; }

bool TaintedRegs::operator==(const TaintedRegs& that) const {
  return m_reg_set == that.m_reg_set;
}

bool TaintedRegs::operator!=(const TaintedRegs& that) const {
  return !(*this == that);
}

void FieldsRegs::meet(const FieldsRegs& that) {
  for (const auto& pair : field_to_reg) {
    if (pair.second == FieldOrRegStatus::DEFAULT) {
      field_to_reg[pair.first] = that.field_to_reg.at(pair.first);
      field_to_iput_insns[pair.first] = that.field_to_iput_insns.at(pair.first);
    } else if (that.field_to_reg.at(pair.first) == FieldOrRegStatus::DEFAULT) {
      continue;
    } else if (pair.second != that.field_to_reg.at(pair.first)) {
      field_to_reg[pair.first] = FieldOrRegStatus::DIFFERENT;
      field_to_iput_insns[pair.first].insert(
          that.field_to_iput_insns.at(pair.first).begin(),
          that.field_to_iput_insns.at(pair.first).end());
    }
  }
}

bool FieldsRegs::operator==(const FieldsRegs& that) const {
  return field_to_reg == that.field_to_reg;
}

bool FieldsRegs::operator!=(const FieldsRegs& that) const {
  return !(*this == that);
}

//////////////////////////////////////////////

bool BuilderTransform::inline_methods(
    DexMethod* method,
    DexType* type,
    std::function<std::vector<DexMethod*>(IRCode*, DexType*)>
        get_methods_to_inline) {
  always_assert(method != nullptr);
  always_assert(type != nullptr);

  auto code = method->get_code();
  if (!code) {
    return false;
  }

  std::vector<DexMethod*> previous_to_inline;
  std::vector<DexMethod*> to_inline = get_methods_to_inline(code, type);

  while (to_inline.size() != 0) {

    m_inliner->inline_callees(method, to_inline);

    // Check all possible methods were inlined.
    previous_to_inline = to_inline;
    to_inline = get_methods_to_inline(code, type);

    // Return false if  nothing changed / nothing got inlined though
    // there were methods to inline.
    if (previous_to_inline == to_inline) {
      return false;
    }
  }

  return true;
}

bool has_builder_name(DexClass* cls) {
  always_assert(cls != nullptr);

  static boost::regex re{"\\$Builder;$"};
  return boost::regex_search(cls->c_str(), re);
}

DexType* get_buildee(DexType* builder) {
  always_assert(builder != nullptr);

  auto builder_name = std::string(builder->c_str());
  auto buildee_name = builder_name.substr(0, builder_name.size() - 9) + ";";
  return DexType::get_type(buildee_name.c_str());
}

bool remove_builder(DexMethod* method, DexClass* builder, DexClass* buildee) {
  always_assert(method != nullptr);
  always_assert(builder != nullptr);
  always_assert(buildee != nullptr);

  auto code = method->get_code();
  if (!code) {
    return false;
  }

  code->build_cfg();
  auto blocks = postorder_sort(code->cfg().blocks());

  auto fields_in = fields_setters(blocks, builder);

  static auto init = DexString::make_string("<init>");
  uint16_t regs_size = code->get_registers_size();
  uint16_t in_regs_size = code->get_ins_size();
  uint16_t non_input_reg_size = regs_size - in_regs_size;
  uint16_t extra_regs = 0;
  int null_reg = FieldOrRegStatus::UNDEFINED;

  std::vector<IRInstruction*> deletes;
  MoveList move_replacements;
  std::unordered_set<IRInstruction*> update_list;

  for (auto& block : blocks) {
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      DexOpcode opcode = insn->opcode();

      auto& fields_in_insn = fields_in->at(mie.insn);

      if (is_iput(opcode)) {
        auto field = insn->get_field();
        if (field->get_class() == builder->get_type()) {
          deletes.push_back(insn);
          continue;
        }

      } else if (is_iget(opcode)) {
        auto field = insn->get_field();
        if (field->get_class() == builder->get_type()) {
          DexOpcode move_opcode = get_move_opcode(insn);
          bool is_wide = move_opcode == OPCODE_MOVE_WIDE;

          if (fields_in_insn.field_to_reg[field] ==
                  FieldOrRegStatus::DIFFERENT ||
              fields_in_insn.field_to_reg[field] ==
                  FieldOrRegStatus::OVERWRITTEN) {

            int new_reg = FieldOrRegStatus::UNDEFINED;
            const auto& iput_insns = fields_in_insn.field_to_iput_insns[field];

            if (iput_insns.size() == 0) {
              return false;
            }

            // Adding a move instruction for each of the setters.
            for (const auto& iput_insn :
                 fields_in_insn.field_to_iput_insns[field]) {

              if (move_replacements.find(iput_insn) !=
                  move_replacements.end()) {
                if (new_reg == FieldOrRegStatus::UNDEFINED) {
                  new_reg = move_replacements[iput_insn]->dest();
                } else {
                  always_assert(new_reg ==
                                move_replacements[iput_insn]->dest());
                }
              } else {
                if (new_reg == FieldOrRegStatus::UNDEFINED) {
                  new_reg = non_input_reg_size + extra_regs;
                  extra_regs += is_wide ? 2 : 1;
                }

                move_replacements[iput_insn] = construct_move_instr(
                    new_reg, iput_insn->src(0), move_opcode);
              }
            }

            always_assert(new_reg != FieldOrRegStatus::UNDEFINED);
            move_replacements[insn] =
                construct_move_instr(insn->dest(), new_reg, move_opcode);

          } else if (fields_in_insn.field_to_reg[field] ==
                     FieldOrRegStatus::UNDEFINED) {

            // We need to add the null one or use it.
            if (null_reg == FieldOrRegStatus::UNDEFINED) {
              null_reg = non_input_reg_size + extra_regs;
              extra_regs++;
            }

            move_replacements[insn] =
                construct_move_instr(insn->dest(), null_reg, OPCODE_MOVE);
          } else {
            // If we got here, the field is held in a register.

            // Get instruction that sets the field.
            const auto& iput_insns = fields_in_insn.field_to_iput_insns[field];
            if (iput_insns.size() == 0) {
              return false;
            }

            always_assert(iput_insns.size() == 1);
            const IRInstruction* iput_insn = *iput_insns.begin();

            // Check if we already have a value for it.
            if (move_replacements.find(iput_insn) != move_replacements.end()) {
              // Get the actual value.
              IRInstruction* new_insn = move_replacements[iput_insn];
              uint16_t new_reg = new_insn->dest();
              move_replacements[insn] =
                  construct_move_instr(insn->dest(), new_reg, move_opcode);

            } else {
              // We can reuse the existing reg, so will have only 1 move.
              //
              // In case this is a parameter reg, it needs to be updated.
              if (iput_insn->src(0) >= non_input_reg_size) {
                update_list.emplace(insn);
              }
              move_replacements[insn] = construct_move_instr(
                  insn->dest(), iput_insn->src(0), move_opcode);
            }
          }

          deletes.push_back(insn);
          continue;
        }

      } else if (opcode == OPCODE_NEW_INSTANCE) {
        DexType* cls = insn->get_type();
        if (type_class(cls) == builder) {
          deletes.push_back(insn);
          continue;
        }

      } else if (is_invoke(opcode)) {
        auto invoked = insn->get_method();
        if (invoked->get_class() == builder->get_type() &&
            invoked->get_name() == init) {
          deletes.push_back(insn);
          continue;
        }
      }
    }
  }

  if (!enlarge_register_frame(method, extra_regs)) {
    return false;
  }

  if (null_reg != FieldOrRegStatus::UNDEFINED) {
    add_null_instr(code, null_reg);
  }

  // Update register parameters.
  update_reg_params(
      update_list, non_input_reg_size, extra_regs, move_replacements);

  method_updates(method, deletes, move_replacements);
  return true;
}
