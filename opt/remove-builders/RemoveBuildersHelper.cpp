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

const IRInstruction* NULL_INSN = nullptr;

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
                          const std::unordered_set<uint16_t>& null_regs) {
  always_assert(code != nullptr);

  auto params = code->get_param_instructions();
  for (uint16_t null_reg : null_regs) {
    code->insert_before(params.end(), construct_null_instr(null_reg));
  }
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

/**
 * Giving a list of setters and a map with instruction replacements,
 * will return an already allocated new register, in case one of the
 * setters already has a replacement defined. Otherwise, it returns
 * UNDEFINED.
 */
int get_new_reg_if_already_allocated(
    const std::unordered_set<const IRInstruction*>& iput_insns,
    MoveList& move_replacements) {

  int new_reg = FieldOrRegStatus::UNDEFINED;
  for (const auto& iput_insn : iput_insns) {
    if (iput_insn != NULL_INSN) {
      if (move_replacements.find(iput_insn) != move_replacements.end()) {
        if (new_reg == FieldOrRegStatus::UNDEFINED) {
          new_reg = move_replacements[iput_insn]->dest();
        } else {
          always_assert(new_reg == move_replacements[iput_insn]->dest());
        }
      }
    }
  }

  return new_reg;
}

/**
 * Check builder's constructor does a small amount of work
 *  - instantiates the parent class (Object)
 *  - returns
 */
bool is_trivial_builder_constructor(DexMethod* method) {
  always_assert(method != nullptr);

  auto code = method->get_code();
  if (!code) {
    return false;
  }

  if (!is_constructor(method)) {
    return false;
  }

  auto ii = InstructionIterable(code);
  auto it = ii.begin();
  if (it->insn->opcode() != IOPCODE_LOAD_PARAM_OBJECT) {
    return false;
  }
  ++it;

  if (it->insn->opcode() != OPCODE_INVOKE_DIRECT) {
    return false;
  } else {
    auto invoked = it->insn->get_method();
    if (!is_constructor(invoked)) {
      return false;
    }
  }

  ++it;
  if (it->insn->opcode() != OPCODE_RETURN_VOID) {
    return false;
  }

  ++it;
  return it == ii.end();
}

std::vector<DexMethod*> get_non_trivial_init_methods(IRCode* code,
                                                     DexType* type) {
  always_assert(code != nullptr);
  always_assert(type != nullptr);

  std::vector<DexMethod*> methods;
  for (auto const& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto invoked = insn->get_method();
      if (invoked->get_class() == type) {
        if (is_constructor(invoked) &&
            !is_trivial_builder_constructor(invoked)) {
          methods.emplace_back(invoked);
        }
      }
    }
  }

  return methods;
}

std::unordered_set<IRInstruction*> get_super_class_initializations(
    DexMethod* method, DexClass* builder) {
  always_assert(method != nullptr);
  always_assert(builder != nullptr);

  std::unordered_set<IRInstruction*> insns;
  auto code = method->get_code();
  if (!code) {
    return insns;
  }

  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto invoked = insn->get_method();
      if (invoked->get_class() == builder->get_super_class() &&
          is_init(invoked)) {
        insns.emplace(insn);
      }
    }
  }

  return insns;
}

bool has_super_class_initializations(DexMethod* method, DexClass* builder) {
  return get_super_class_initializations(method, builder).size() != 0;
}

void remove_super_class_calls(DexMethod* method, DexClass* builder) {
  std::unordered_set<IRInstruction*> to_delete =
      get_super_class_initializations(method, builder);
  auto code = method->get_code();
  if (!code) {
    return;
  }

  for (const auto& insn : to_delete) {
    code->remove_opcode(insn);
  }
}

/**
 * Gathers all `MOVE` instructions that operate on a builder.
 */
std::vector<IRInstruction*> gather_move_builders_insn(
    IRCode* code, const std::vector<Block*>& blocks, DexType* builder) {
  std::vector<IRInstruction*> insns;

  uint16_t regs_size = code->get_registers_size();
  std::function<void(const IRInstruction*, TaintedRegs*)> trans = [&](
      const IRInstruction* insn, TaintedRegs* tregs) {
    auto& regs = tregs->m_reg_set;
    auto op = insn->opcode();
    if (op == OPCODE_NEW_INSTANCE) {
      DexType* cls = insn->get_type();
      if (cls == builder) {
        regs[insn->dest()] = 1;
      }
    } else {
      transfer_object_reach(builder, regs_size, insn, tregs->m_reg_set);
    }
  };

  // Keep track, per instruction, what register(s) holds a builder.
  // The extra register is used to keep track of return values.
  auto tainted_map =
      forwards_dataflow(blocks, TaintedRegs(regs_size + 1), trans);

  for (auto it : *tainted_map) {
    auto insn = it.first;
    auto tainted = it.second.bits();

    if (is_move(insn->opcode())) {
      if (tainted[insn->src(0)]) {
        insns.push_back(insn);
      }
    }
  }

  return insns;
}

bool remove_builder(DexMethod* method, DexClass* builder) {
  always_assert(method != nullptr);
  always_assert(builder != nullptr);

  auto code = method->get_code();
  if (!code) {
    return false;
  }

  code->build_cfg();
  auto blocks = postorder_sort(code->cfg().blocks());

  auto fields_in = fields_setters(blocks, builder);

  static auto init = DexString::make_string("<init>");
  uint16_t regs_size = code->get_registers_size();
  uint16_t in_regs_size = sum_param_sizes(code);
  uint16_t non_input_reg_size = regs_size - in_regs_size;
  uint16_t extra_regs = 0;
  int null_reg = FieldOrRegStatus::UNDEFINED;
  std::unordered_set<uint16_t> extra_null_regs;

  // Instructions where the builder gets moved to a different
  // register need to be also removed (at the end).
  std::vector<IRInstruction*> deletes =
      gather_move_builders_insn(code, blocks, builder->get_type());
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

            const auto& iput_insns = fields_in_insn.field_to_iput_insns[field];
            always_assert(iput_insns.size() > 0);

            int new_reg =
                get_new_reg_if_already_allocated(iput_insns, move_replacements);
            if (new_reg == FieldOrRegStatus::UNDEFINED) {
              // Allocating a new register since one was not allocated.
              new_reg = non_input_reg_size + extra_regs;
              extra_regs += is_wide ? 2 : 1;
            }

            for (const auto& iput_insn : iput_insns) {
              if (iput_insn != NULL_INSN) {
                if (move_replacements.find(iput_insn) !=
                    move_replacements.end()) {
                  always_assert(new_reg ==
                                move_replacements[iput_insn]->dest());
                } else {
                  // Adding a move for each of the setters:
                  //   iput v1, object // field -> move new_reg, v1
                  move_replacements[iput_insn] = construct_move_instr(
                      new_reg, iput_insn->src(0), move_opcode);
                }
              } else {
                // Initializes the register since the field might be
                // uninitialized.
                extra_null_regs.emplace(new_reg);
              }
            }

            // Adding a move for the getter:
            //   iget v2, object // field -> move v2, new_reg
            move_replacements[insn] =
                construct_move_instr(insn->dest(), new_reg, move_opcode);

          } else if (fields_in_insn.field_to_reg[field] ==
                     FieldOrRegStatus::UNDEFINED) {

            // Initializing the field with null. Reusing the 'null_reg' if
            // already defined.
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
    extra_null_regs.emplace(null_reg);
  }
  null_initializations(code, extra_null_regs);

  // Update register parameters.
  update_reg_params(
      update_list, non_input_reg_size, extra_regs, move_replacements);

  method_updates(method, deletes, move_replacements);
  return true;
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
      if (pair.second == FieldOrRegStatus::UNDEFINED ||
          that.field_to_reg.at(pair.first) == FieldOrRegStatus::UNDEFINED) {
        field_to_iput_insns[pair.first].insert(NULL_INSN);
      }

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

void transfer_object_reach(DexType* obj,
                           uint16_t regs_size,
                           const IRInstruction* insn,
                           RegSet& regs) {
  always_assert(obj != nullptr);
  always_assert(insn != nullptr);

  auto op = insn->opcode();
  if (is_move(op)) {
    regs[insn->dest()] = regs[insn->src(0)];
    if (insn->src_is_wide(0)) {
      regs[insn->dest() + 1] = regs[insn->src(0)];
    }
  } else if (is_move_result(op)) {
    regs[insn->dest()] = regs[regs_size];
  } else if (writes_result_register(op)) {
    if (is_invoke(op)) {
      auto invoked = insn->get_method();
      if (invoked->get_proto()->get_rtype() == obj) {
        regs[regs_size] = 1;
        return;
      }
    }
    regs[regs_size] = 0;
  } else if (insn->dests_size() != 0) {
    regs[insn->dest()] = 0;
    if (insn->dest_is_wide()) {
      regs[insn->dest() + 1] = 0;
    }
  }
}

//////////////////////////////////////////////

bool has_builder_name(DexType* cls) {
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

std::vector<DexMethod*> get_all_methods(IRCode* code, DexType* type) {
  always_assert(code != nullptr);
  always_assert(type != nullptr);

  std::vector<DexMethod*> methods;
  for (auto const& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto invoked = insn->get_method();
      if (invoked->get_class() == type) {
        methods.emplace_back(invoked);
      }
    }
  }

  return methods;
}

std::vector<DexMethod*> get_non_init_methods(IRCode* code, DexType* type) {
  std::vector<DexMethod*> methods = get_all_methods(code, type);
  methods.erase(
      remove_if(methods.begin(),
                methods.end(),
                [&](DexMethod* m) { return is_init(m); }),
      methods.end());

  return methods;
}

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

bool remove_builder_from(DexMethod* method,
                         DexClass* builder,
                         BuilderTransform& b_transform) {
  DexType* buildee = get_buildee(builder->get_type());
  always_assert(buildee != nullptr);

  DexMethod* method_copy = nullptr;
  bool remove_failed = false;

  while (!remove_failed &&
         get_non_trivial_init_methods(method->get_code(), builder->get_type())
                 .size() > 0) {

    // Filter out builders for which the method contains super class invokes.
    if (has_super_class_initializations(method, builder)) {
      return false;
    }

    // Keeping a copy of the method in case we need to restore it.
    if (!method_copy) {
      method_copy = DexMethod::make_method_from(
          method,
          method->get_class(),
          DexString::make_string(std::string(method->get_name()->c_str()) +
                                 "$redex"));
    }

    if (!b_transform.inline_methods(
            method, builder->get_type(), &get_non_trivial_init_methods)) {
      remove_failed = true;
    } else {
      // Inline remaining calls.
      if (!b_transform.inline_methods(
              method, builder->get_type(), &get_non_init_methods)) {
        remove_failed = true;
      }
    }
  }

  if (remove_failed || !remove_builder(method, builder)) {
    remove_failed = true;
  }

  if (method_copy) {
    if (remove_failed) {
      // Restore method, since without removing the builder we need the
      // initializations.
      method->set_code(method_copy->release_code());
    } else {
      // Cleanup after constructor inlining.
      remove_super_class_calls(method, builder);
    }

    DexMethod::erase_method(method_copy);
  }

  return !remove_failed;
}
