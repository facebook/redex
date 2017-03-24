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

  // Set DEFAULT fields to UNDEFINED.
  for (auto& pair : fregs->field_to_reg) {
    if (pair.second == FieldOrRegStatus::DEFAULT) {
      fregs->field_to_reg[pair.first] = FieldOrRegStatus::UNDEFINED;
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
    auto field = static_cast<const IRFieldInstruction*>(insn)->field();

    if (field->get_class() == builder->get_type()) {
      uint16_t current = is_setter ? insn->src(0) : insn->dest();
      fregs->field_to_reg[field] = current;
      if (is_setter) {
        fregs->field_to_iput_insn[field] = insn;
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

  auto& code = method->get_code();
  auto oldregs = code->get_registers_size();
  auto newregs = oldregs + extra_regs;

  return MethodTransform::enlarge_regs(method, newregs);
}

DexOpcode get_move_opcode(const IRInstruction* insn) {
  always_assert(insn != nullptr);
  always_assert(is_iput(insn->opcode()));

  if (insn->opcode() == OPCODE_IPUT_WIDE) {
    return OPCODE_MOVE_WIDE;
  } else if (insn->opcode() == OPCODE_IPUT_OBJECT) {
    return OPCODE_MOVE_OBJECT;
  }

  return OPCODE_MOVE;
}

/**
 * Adds an instruction that initializes a new register with null.
 */
void add_null_instr(MethodTransform* transform, uint16_t reg) {
  always_assert(transform != nullptr);

  IRInstruction* insn = new IRInstruction(OPCODE_CONST_4);

  insn->set_dest(reg);
  insn->set_literal(0);

  std::vector<IRInstruction*> insns;
  insns.push_back(insn);

  // Adds the instruction at the beginning, since it might be
  // used in various places later.
  transform->insert_after(nullptr, insns);
}

/**
 * Adds a move instruction after the given instruction.
 */
void add_move_instr(MethodTransform* transform,
                    const IRInstruction* position,
                    uint16_t src_reg,
                    uint16_t dest_reg,
                    DexOpcode move_opcode) {

  always_assert(transform != nullptr);
  always_assert(position != nullptr);

  IRInstruction* insn = new IRInstruction(move_opcode);
  insn->set_dest(dest_reg);
  insn->set_src(0, src_reg);

  std::vector<IRInstruction*> insns;
  insns.push_back(insn);

  transform->insert_after(const_cast<IRInstruction*>(position), insns);
}

using MoveList = std::unordered_map<
    const IRInstruction*,
    std::tuple</* dest reg */ uint16_t, /* src reg */ uint16_t, DexOpcode>>;

void method_updates(DexMethod* method,
                    const std::vector<IRInstruction*>& deletes,
                    const MoveList& move_list,
                    uint16_t initial_non_input_reg_size,
                    uint16_t extra_regs) {

  auto& code = method->get_code();
  auto transform = code->get_entries();

  for (const auto& move_elem : move_list) {
    const IRInstruction* insn = move_elem.first;

    uint16_t dest_reg = std::get<0>(move_elem.second);
    uint16_t src_reg = std::get<1>(move_elem.second);

    // If we are moving parameters around we need to account
    // for the extra allocated regs.
    if (is_iput(insn->opcode())) {
      if (src_reg >= initial_non_input_reg_size) {
        src_reg += extra_regs;
      }
    } else if (dest_reg >= initial_non_input_reg_size) {
      dest_reg += extra_regs;
    }

    DexOpcode move_opcode = std::get<2>(move_elem.second);

    add_move_instr(transform, insn, src_reg, dest_reg, move_opcode);
  }

  for (const auto& insn : deletes) {
    transform->remove_opcode(insn);
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
      field_to_iput_insn[pair.first] = that.field_to_iput_insn.at(pair.first);
    } else if (that.field_to_reg.at(pair.first) == FieldOrRegStatus::DEFAULT) {
      continue;
    } else if (pair.second != that.field_to_reg.at(pair.first)) {
      field_to_reg[pair.first] = FieldOrRegStatus::DIFFERENT;
      field_to_iput_insn[pair.first] = nullptr;
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

DexMethod* get_build_method(const std::vector<DexMethod*>& vmethods) {
  static auto build = DexString::make_string("build");
  for (const auto& vmethod : vmethods) {
    if (vmethod->get_name() == build) {
      return vmethod;
    }
  }

  return nullptr;
}

bool inline_build(DexMethod* method, DexClass* builder) {
  auto& code = method->get_code();
  if (!code) {
    return false;
  }

  std::vector<std::pair<DexMethod*, IRMethodInstruction*>> inlinables;
  DexMethod* build_method = get_build_method(builder->get_vmethods());

  for (auto const& mie : InstructionIterable(code->get_entries())) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto invoked =
          static_cast<const IRMethodInstruction*>(insn)->get_method();
      if (invoked == build_method) {
        auto mop = static_cast<IRMethodInstruction*>(insn);
        inlinables.push_back(std::make_pair(build_method, mop));
      }
    }
  }

  // For the moment, not treating the case where we have 2 instances
  // of the same builder.
  if (inlinables.size() > 1) {
    return false;
  }

  InlineContext inline_context(method, false);
  for (auto inlinable : inlinables) {
    // TODO(emmasevastian): We will need to gate this with a check, mostly as
    //                      we loosen the build method restraints.
    if (!MethodTransform::inline_16regs(
            inline_context, inlinable.first, inlinable.second)) {
      return false;
    }
  }

  return true;
}

bool remove_builder(DexMethod* method, DexClass* builder, DexClass* buildee) {
  auto& code = method->get_code();
  if (!code) {
    return false;
  }

  auto transform = code->get_entries();
  transform->build_cfg();
  auto blocks = postorder_sort(transform->cfg().blocks());

  auto fields_in = fields_setters(blocks, builder);

  static auto init = DexString::make_string("<init>");
  uint16_t regs_size = code->get_registers_size();
  uint16_t in_regs_size = code->get_ins_size();
  uint16_t non_input_reg_size = regs_size - in_regs_size;
  uint16_t extra_regs = 0;
  int null_reg = FieldOrRegStatus::UNDEFINED;

  std::vector<IRInstruction*> deletes;
  MoveList move_replacements;

  for (auto& block : blocks) {
    for (auto& mie : *block) {
      if (mie.type != MFLOW_OPCODE) {
        continue;
      }

      auto insn = mie.insn;
      DexOpcode opcode = insn->opcode();

      auto& fields_in_insn = fields_in->at(mie.insn);

      if (is_iput(opcode)) {
        auto field = static_cast<const IRFieldInstruction*>(insn)->field();
        if (field->get_class() == builder->get_type()) {
          deletes.push_back(insn);
          continue;
        }

      } else if (is_iget(opcode)) {
        auto field = static_cast<const IRFieldInstruction*>(insn)->field();
        if (field->get_class() == builder->get_type()) {

          // Not treating the cases where we are not sure how the field
          // was initialized.
          if (fields_in_insn.field_to_reg[field] ==
              FieldOrRegStatus::DIFFERENT) {
            return false;

          } else if (fields_in_insn.field_to_reg[field] ==
                     FieldOrRegStatus::UNDEFINED) {

            // We need to add the null one or use it.
            if (null_reg == FieldOrRegStatus::UNDEFINED) {
              null_reg = non_input_reg_size + extra_regs;
              extra_regs++;
            }

            move_replacements[insn] =
                std::make_tuple(insn->dest(), null_reg, OPCODE_MOVE);

          } else {
            // If we got here, the field is either:
            //   OVERWRITTEN or held in a register.

            // Get instruction that sets the field.
            const IRInstruction* iput_insn =
                fields_in_insn.field_to_iput_insn[field];
            if (iput_insn == nullptr) {
              return false;
            }

            DexOpcode move_opcode = get_move_opcode(iput_insn);
            bool is_wide = move_opcode == OPCODE_MOVE_WIDE;

            // Check if we already have a value for it.
            if (move_replacements.find(iput_insn) != move_replacements.end()) {
              move_replacements[insn] =
                  std::make_tuple(insn->dest(),
                                  std::get<0>(move_replacements[iput_insn]),
                                  move_opcode);

            } else if (fields_in_insn.field_to_reg[field] ==
                       FieldOrRegStatus::OVERWRITTEN) {

              // We need to add 2 moves: one for the iput, one for iget.
              move_replacements[iput_insn] =
                  std::make_tuple(non_input_reg_size + extra_regs,
                                  iput_insn->src(0),
                                  move_opcode);

              move_replacements[insn] = std::make_tuple(
                  insn->dest(), non_input_reg_size + extra_regs, move_opcode);

              extra_regs += is_wide ? 2 : 1;

            } else {
              // We can reuse the existing reg, so will have only 1 move.
              move_replacements[insn] =
                  std::make_tuple(insn->dest(), iput_insn->src(0), move_opcode);
            }
          }

          deletes.push_back(insn);
          continue;
        }

      } else if (opcode == OPCODE_NEW_INSTANCE) {
        DexType* cls = static_cast<IRTypeInstruction*>(insn)->get_type();
        if (type_class(cls) == builder) {
          deletes.push_back(insn);
          continue;
        }

      } else if (is_invoke(opcode)) {
        auto invoked =
            static_cast<const IRMethodInstruction*>(insn)->get_method();
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
    add_null_instr(transform, null_reg);
  }

  method_updates(
      method, deletes, move_replacements, non_input_reg_size, extra_regs);
  return true;
}
