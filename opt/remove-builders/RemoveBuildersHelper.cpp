/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveBuildersHelper.h"

#include <boost/dynamic_bitset.hpp>
#include <boost/regex.hpp>

#include "ControlFlow.h"
#include "Dataflow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"

namespace {

const IRInstruction* NULL_INSN = nullptr;

void fields_mapping(IRList::iterator it,
                    FieldsRegs* fregs,
                    DexClass* builder) {
  always_assert(fregs != nullptr);
  always_assert(builder != nullptr);
  always_assert(it->type == MFLOW_OPCODE);

  const IRInstruction* insn = it->insn;

  if (insn->opcode() == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT &&
      std::prev(it)->insn->opcode() == OPCODE_NEW_INSTANCE &&
      std::prev(it)->insn->get_type() == builder->get_type()) {
    // Set fields to UNDEFINED if new builder instance.
    for (auto& pair : fregs->field_to_reg) {
      fregs->field_to_reg[pair.first] = FieldOrRegStatus::UNDEFINED;
      fregs->field_to_iput_insns[pair.first].clear();
    }
  }

  // Check if the register that used to hold the field's value is overwritten.
  if (insn->dests_size()) {
    const int current_dest = insn->dest();

    for (const auto& pair : fregs->field_to_reg) {
      if (pair.second == current_dest ||
          (insn->dest_is_wide() && pair.second == current_dest + 1)) {
        fregs->field_to_reg[pair.first] = FieldOrRegStatus::OVERWRITTEN;
      }
    }
  }

  if (is_iput(insn->opcode())) {
    auto field = resolve_field(insn->get_field(), FieldSearch::Instance);

    if (field != nullptr && field->get_class() == builder->get_type()) {
      uint16_t current = insn->src(0);
      fregs->field_to_reg[field] = current;
      fregs->field_to_iput_insns[field].clear();
      fregs->field_to_iput_insns[field].emplace(insn);
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
    const std::vector<cfg::Block*>& blocks, DexClass* builder) {

  std::function<void(IRList::iterator, FieldsRegs*)> trans = [&](
      IRList::iterator it, FieldsRegs* fregs) {
    fields_mapping(it, fregs, builder);
  };

  return forwards_dataflow(blocks, FieldsRegs(builder), trans);
}

IROpcode get_move_opcode(const IRInstruction* insn) {
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
                                    IROpcode move_opcode) {
  IRInstruction* insn = new IRInstruction(move_opcode);
  insn->set_dest(dest_reg);
  insn->set_src(0, src_reg);
  return insn;
}

IRInstruction* construct_null_instr(uint16_t reg, IROpcode move_opcode) {
  IRInstruction* insn;
  if (move_opcode == OPCODE_MOVE_WIDE) {
    insn = new IRInstruction(OPCODE_CONST_WIDE);
  } else {
    insn = new IRInstruction(OPCODE_CONST);
  }
  insn->set_dest(reg);
  insn->set_literal(0);
  return insn;
}

/**
 * Adds instructions that initializes registers with null.
 */
void null_initializations(
    IRCode* code, const std::vector<std::pair<uint16_t, IROpcode>>& null_regs) {
  always_assert(code != nullptr);

  auto params = code->get_param_instructions();
  for (auto& null_reg_info : null_regs) {
    uint16_t null_reg = null_reg_info.first;
    IROpcode move_opcode = null_reg_info.second;
    code->insert_before(params.end(),
                        construct_null_instr(null_reg, move_opcode));
  }
}

void add_instr(IRCode* code,
               const IRInstruction* position,
               IRInstruction* new_insn) {
  always_assert(code != nullptr);
  always_assert(position != nullptr);
  always_assert(new_insn != nullptr);

  for (auto it = code->begin(); it != code->end(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    auto insn = it->insn;
    if (insn == position) {
      code->insert_before(it, new_insn);
      return;
    }
  }
  always_assert_log(false, "insert position not found!");
}

using MoveList = std::unordered_map<const IRInstruction*, IRInstruction*>;

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
    auto invoked = resolve_method(it->insn->get_method(), MethodSearch::Direct);
    if (invoked == nullptr || !is_constructor(invoked)) {
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
      auto invoked = resolve_method(insn->get_method(), opcode_to_search(insn));
      if (invoked != nullptr && invoked->get_class() == type) {
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
    DexMethod* method, DexType* parent_type) {
  always_assert(method != nullptr);
  always_assert(parent_type != nullptr);

  std::unordered_set<IRInstruction*> insns;
  auto code = method->get_code();
  if (!code) {
    return insns;
  }

  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (is_invoke(insn->opcode())) {
      auto invoked = resolve_method(insn->get_method(), opcode_to_search(insn));
      if (invoked != nullptr && invoked->get_class() == parent_type &&
          is_init(invoked)) {
        insns.emplace(insn);
      }
    }
  }

  return insns;
}

bool has_super_class_initializations(DexMethod* method, DexType* parent_type) {
  return get_super_class_initializations(method, parent_type).size() != 0;
}

void remove_super_class_calls(DexMethod* method, DexType* parent_type) {
  std::unordered_set<IRInstruction*> to_delete =
      get_super_class_initializations(method, parent_type);
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
    IRCode* code, const std::vector<cfg::Block*>& blocks, DexType* builder) {
  std::vector<IRInstruction*> insns;

  uint16_t regs_size = code->get_registers_size();
  auto tainted_map = get_tainted_regs(regs_size, blocks, builder);

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

/**
 * Keeps tracks of registers that are going to be used for undefined fields
 * depending on the type of field: wide, primitive etc.
 */
class ZeroRegs {
 public:
  bool has(DexType* type) { return get(type) != FieldOrRegStatus::UNDEFINED; }

  uint16_t get(DexType* type, uint16_t default_value) {
    if (!has(type)) {
      set(type, default_value);
    }
    return get(type);
  }

 private:
  int m_zero_reg_object{FieldOrRegStatus::UNDEFINED};
  int m_zero_reg_int{FieldOrRegStatus::UNDEFINED};
  int m_zero_reg_float{FieldOrRegStatus::UNDEFINED};
  int m_zero_reg_long{FieldOrRegStatus::UNDEFINED};
  int m_zero_reg_double{FieldOrRegStatus::UNDEFINED};

  int get(DexType* type) {
    const auto* name = type->get_name()->c_str();
    switch (name[0]) {
    case 'Z':
    case 'B':
    case 'S':
    case 'C':
    case 'I':
      return m_zero_reg_int;
    case 'J':
      return m_zero_reg_long;
    case 'F':
      return m_zero_reg_float;
    case 'D':
      return m_zero_reg_double;
    case 'L':
    case '[':
      return m_zero_reg_object;
    default:
      not_reached();
    }
  }

  void set(DexType* type, uint16_t value) {
    const auto* name = type->get_name()->c_str();
    switch (name[0]) {
    case 'Z':
    case 'B':
    case 'S':
    case 'C':
    case 'I':
      m_zero_reg_int = value;
      return;
    case 'J':
      m_zero_reg_long = value;
      return;
    case 'F':
      m_zero_reg_float = value;
      return;
    case 'D':
      m_zero_reg_double = value;
      return;
    case 'L':
    case '[':
      m_zero_reg_object = value;
      return;
    default:
      not_reached();
    }
  }
};

bool remove_builder(DexMethod* method, DexClass* builder) {
  always_assert(method != nullptr);
  always_assert(builder != nullptr);

  auto code = method->get_code();
  if (!code) {
    return false;
  }

  code->build_cfg(/* editable */ false);
  auto blocks = cfg::postorder_sort(code->cfg().blocks());
  std::reverse(blocks.begin(), blocks.end());

  auto fields_in = fields_setters(blocks, builder);

  static auto init = DexString::make_string("<init>");
  uint16_t regs_size = code->get_registers_size();
  uint16_t next_available_reg = regs_size;
  uint16_t extra_regs = 0;
  size_t num_builders = 0;
  std::vector<std::pair<uint16_t, IROpcode>> extra_null_regs;
  ZeroRegs undef_fields_regs;

  // Instructions where the builder gets moved to a different
  // register need to be also removed (at the end).
  std::vector<IRInstruction*> deletes =
      gather_move_builders_insn(code, blocks, builder->get_type());
  MoveList move_replacements;
  std::unordered_set<IRInstruction*> update_list;

  for (auto& block : blocks) {
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto insn = it->insn;
      IROpcode opcode = insn->opcode();

      auto& fields_in_insn = fields_in->at(it->insn);

      if (is_iput(opcode)) {
        auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
        if (field != nullptr && field->get_class() == builder->get_type()) {
          deletes.push_back(insn);
          continue;
        }

      } else if (is_iget(opcode)) {
        auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
        if (field == nullptr) continue;
        if (field->get_class() == builder->get_type()) {
          IROpcode move_opcode = get_move_opcode(insn);
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
              new_reg = next_available_reg + extra_regs;
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
                extra_null_regs.push_back(std::make_pair(new_reg, move_opcode));
              }
            }

            // Adding a move for the getter:
            //   iget v2, object // field -> move v2, new_reg
            move_replacements[insn] = construct_move_instr(
                std::next(it)->insn->dest(), new_reg, move_opcode);

          } else if (fields_in_insn.field_to_reg[field] ==
                     FieldOrRegStatus::UNDEFINED) {

            // Initializing the field with null.
            bool has_null_reg = undef_fields_regs.has(field->get_type());
            uint16_t new_null_reg = undef_fields_regs.get(
                field->get_type(), next_available_reg + extra_regs);

            move_replacements[insn] = construct_move_instr(
                std::next(it)->insn->dest(), new_null_reg, move_opcode);

            if (!has_null_reg) {
              extra_null_regs.emplace_back(new_null_reg, move_opcode);
              extra_regs += is_wide ? 2 : 1;
            }
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
              move_replacements[insn] = construct_move_instr(
                  std::next(it)->insn->dest(), new_reg, move_opcode);

            } else {
              // We can reuse the existing reg, so will have only 1 move.
              //
              // In case this is a parameter reg, it needs to be updated.
              if (iput_insn->src(0) >= next_available_reg) {
                update_list.emplace(insn);
              }
              move_replacements[insn] = construct_move_instr(
                  std::next(it)->insn->dest(), iput_insn->src(0), move_opcode);
            }
          }

          deletes.push_back(insn);
          continue;
        }

      } else if (opcode == OPCODE_NEW_INSTANCE ||
                 opcode == OPCODE_CHECK_CAST) {
        DexType* cls = insn->get_type();
        if (type_class(cls) == builder) {
          if (opcode == OPCODE_NEW_INSTANCE) num_builders++;

          // Safely avoiding the case where multiple builders are initialized.
          if (num_builders > 1) return false;

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

  code->set_registers_size(next_available_reg + extra_regs);

  null_initializations(code, extra_null_regs);

  method_updates(method, deletes, move_replacements);
  return true;
}

bool has_only_argument(DexMethod* method, DexType* type) {
  DexProto* proto = method->get_proto();
  const auto& args = proto->get_args()->get_type_list();
  if (args.size() != 1 || args[0] != type) {
    return false;
  }

  return true;
}

IROpcode get_iget_type(DexField* field) {
  switch (type_to_datatype(field->get_type())) {
  case DataType::Array:
  case DataType::Object:
    return OPCODE_IGET_OBJECT;
  case DataType::Boolean:
    return OPCODE_IGET_BOOLEAN;
  case DataType::Byte:
    return OPCODE_IGET_BYTE;
  case DataType::Char:
    return OPCODE_IGET_CHAR;
  case DataType::Short:
    return OPCODE_IGET_SHORT;
  case DataType::Int:
  case DataType::Float:
    return OPCODE_IGET;
  case DataType::Long:
  case DataType::Double:
    return OPCODE_IGET_WIDE;
  case DataType::Void:
    redex_assert(false);
  }
  not_reached();
}

/**
 * Checks if the registers which hold the arguments for the given method
 * are used as source for any operation except `iget-*`
 */
bool params_change_regs(DexMethod* method) {
  DexProto* proto = method->get_proto();
  auto args = proto->get_args()->get_type_list();

  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto blocks = cfg::postorder_sort(code->cfg().blocks());
  std::reverse(blocks.begin(), blocks.end());
  uint16_t regs_size = code->get_registers_size();
  const auto& param_insns =
      InstructionIterable(code->get_param_instructions());
  always_assert(!is_static(method));
  // Skip the `this` param
  auto param_it = std::next(param_insns.begin());

  for (DexType* arg : args) {
    std::function<void(IRList::iterator, TaintedRegs*)> trans =
        [&](IRList::iterator it, TaintedRegs* tregs) {
          if (!opcode::is_load_param(it->insn->opcode())) {
            transfer_object_reach(arg, regs_size, it->insn, tregs->m_reg_set);
          }
        };

    auto tainted = TaintedRegs(regs_size + 1);
    always_assert(param_it != param_insns.end());
    auto arg_reg = (param_it++)->insn->dest();
    tainted.m_reg_set[arg_reg] = 1;

    auto taint_map = forwards_dataflow(blocks, tainted, trans);
    for (auto it : *taint_map) {
      auto insn = it.first;
      auto insn_tainted = it.second.bits();
      auto op = insn->opcode();

      if (opcode::is_load_param(op)) {
        continue;
      }

      if (is_iget(op)) {
        DexField* field =
            resolve_field(insn->get_field(), FieldSearch::Instance);
        if (field->get_class() == arg) {
          continue;
        }
      }

      if (insn->dests_size() && insn_tainted[insn->dest()]) {
        return true;
      }
      for (size_t index = 0; index < insn->srcs_size(); ++index) {
        if (insn_tainted[insn->src(index)]) {
          return true;
        }
      }
    }
  }

  return false;
}

/**
 *  Creates a DexProto starting from the ifields of the class.
 *  Example: (field1_type, field2_type ...)V;
 */
DexProto* make_proto_for(DexClass* cls) {
  const auto& fields = cls->get_ifields();

  std::deque<DexType*> dfields;
  for (const DexField* field : fields) {
    dfields.push_back(field->get_type());
  }

  auto fields_list = DexTypeList::make_type_list(std::move(dfields));
  return DexProto::make_proto(get_void_type(), fields_list);
}

/**
 * Generate load params instructions for a non-static method with
 * the `field` arguments.
 *
 * At the same time, update field to register mapping.
 */
std::vector<IRInstruction*> generate_load_params(
    const std::vector<DexField*>& fields,
    uint16_t& params_reg_start,
    std::unordered_map<DexField*, uint16_t>& field_to_reg) {

  std::vector<IRInstruction*> load_params;

  // Load current instance.
  IRInstruction* insn = new IRInstruction(IOPCODE_LOAD_PARAM_OBJECT);
  insn->set_dest(params_reg_start++);
  load_params.push_back(insn);

  for (DexField* field : fields) {
    IROpcode op;
    if (is_wide_type(field->get_type())) {
      op = IOPCODE_LOAD_PARAM_WIDE;
    } else {
      op = is_primitive(field->get_type()) ? IOPCODE_LOAD_PARAM
                                           : IOPCODE_LOAD_PARAM_OBJECT;
    }

    insn = new IRInstruction(op);
    insn->set_dest(params_reg_start);
    field_to_reg[field] = params_reg_start;
    params_reg_start += is_wide_type(field->get_type()) ? 2 : 1;
    load_params.push_back(insn);
  }

  return load_params;
}

/**
 * Given a method that takes cls as an argument, creates a new method
 * that takes cls's fields as arguments.
 */
DexMethod* create_fields_constr(DexMethod* method, DexClass* cls) {
  auto init = DexString::get_string("<init>");
  auto void_fields = make_proto_for(cls);
  DexMethod* fields_constr = static_cast<DexMethod*>(DexMethod::make_method(
      method->get_class(), init, void_fields));
  fields_constr->make_concrete(ACC_PUBLIC | ACC_CONSTRUCTOR, false);

  auto code = method->get_code();
  uint16_t regs_size = code->get_registers_size();
  const auto& fields = cls->get_ifields();
  std::unordered_map<DexField*, uint16_t> field_to_reg;

  std::unique_ptr<IRCode> new_code = std::make_unique<IRCode>(*code);

  // Non-input registers for the method are all registers except the
  // 'this' register and the arguments (which in this case is just 1)
  uint16_t new_regs_size = regs_size - 2;
  std::vector<IRInstruction*> load_params = generate_load_params(
      fields, new_regs_size, field_to_reg);
  new_code->set_registers_size(new_regs_size);

  std::vector<IRList::iterator> to_delete;
  std::unordered_map<IRInstruction*, IRInstruction*> to_replace;
  auto ii = InstructionIterable(*new_code);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    IRInstruction* insn = it->insn;

    // Delete old parameter loads.
    if (opcode::is_load_param(insn->opcode())) {
      to_delete.emplace_back(it.unwrap());
      continue;
    }

    if (is_iget(insn->opcode())) {
      DexField* field = resolve_field(insn->get_field(), FieldSearch::Instance);
      if (field->get_class() == cls->get_type()) {

        // Replace `iget <v_dest>, <v_builder>` with `move <v_dest>, <v_field>`
        uint16_t current_reg = std::next(it)->insn->dest();
        IROpcode move_opcode = get_move_opcode(insn);
        auto* move = new IRInstruction(move_opcode);
        move->set_src(0, field_to_reg[field]);
        move->set_dest(current_reg);
        to_replace.emplace(insn, move);
      }
    }
  }

  new_code->insert_after(nullptr, load_params);
  for (const auto& it : to_replace) {
    new_code->replace_opcode(it.first, it.second);
  }
  for (const auto& it : to_delete) {
    new_code->erase(it);
  }

  fields_constr->set_code(std::move(new_code));
  type_class(method->get_class())->add_method(fields_constr);
  return fields_constr;
}

DexMethodRef* get_fields_constr_if_exists(DexMethod* method, DexClass* cls) {
  DexType* type = method->get_class();
  auto void_fields = make_proto_for(cls);
  auto init = DexString::get_string("<init>");
  return DexMethod::get_method(type, init, void_fields);
}

DexMethod* get_fields_constr(DexMethod* method, DexClass* cls) {
  DexMethodRef* fields_constr = get_fields_constr_if_exists(method, cls);
  if (!fields_constr || !fields_constr->is_def()) {
    return create_fields_constr(method, cls);
  }

  return static_cast<DexMethod*>(fields_constr);
}

std::vector<IRList::iterator> get_invokes_for_method(IRCode* code,
                                                        DexMethod* method) {
  std::vector<IRList::iterator> fms;
  auto ii = InstructionIterable(code);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    if (is_invoke(insn->opcode())) {
      auto invoked = insn->get_method();
      auto def = resolve_method(invoked, MethodSearch::Any);
      if (def) {
        invoked = def;
      }

      if (invoked == method) {
        fms.emplace_back(it.unwrap());
      }
    }
  }

  return fms;
}

/**
 * For the cases where the buildee accepts the builder as the only argument, we
 * create a new constructor, that will take all the builder's fields as arguments.
 */
bool update_buildee_constructor(DexMethod* method, DexClass* builder) {
  DexType* buildee = get_buildee(builder->get_type());

  DexMethodRef* buildee_constr_ref = DexMethod::get_method(
      buildee,
      DexString::make_string("<init>"),
      DexProto::make_proto(
        get_void_type(),
        DexTypeList::make_type_list({builder->get_type()})));
  if (!buildee_constr_ref || !buildee_constr_ref->is_def()) {
    // Nothing to search for.
    return true;
  }
  DexMethod* buildee_constr = static_cast<DexMethod*>(buildee_constr_ref);

  // Extra conservative: We expect the constructor to do minimum work.
  if (params_change_regs(buildee_constr)) {
    return false;
  }

  auto code = method->get_code();
  std::vector<IRList::iterator> buildee_constr_calls =
    get_invokes_for_method(code, buildee_constr);
  if (buildee_constr_calls.size()) {

    DexMethod* fields_constr = get_fields_constr(buildee_constr, builder);
    if (!fields_constr) {
      return false;
    }

    for (const auto& it : buildee_constr_calls) {
      IRInstruction* insn = it->insn;
      uint16_t builder_reg = insn->src(1);
      uint16_t regs_size = code->get_registers_size();
      uint16_t new_regs_size = regs_size;

      auto fields = builder->get_ifields();
      insn->set_method(fields_constr);
      // 'Make room' for the reg arguments.
      insn->set_arg_word_count(2 * fields.size() + 1);

      // Loading each of the fields before passing them to the method.
      // `invoke-direct {v_class, v_builder}` ->
      //    `iget v_field_1, v_builder
      //    iget v_field_2, v_builder
      //    ....
      //    invoke_direct {v_class, v_field_1, v_field_2, ...}`
      uint16_t index = 1;
      for (DexField* field : fields) {
        auto* new_insn = new IRInstruction(get_iget_type(field));
        new_insn->set_src(0, builder_reg);
        new_insn->set_field(field);
        code->insert_before(it, new_insn);
        auto* move_result_pseudo =
            new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        move_result_pseudo->set_dest(new_regs_size);
        code->insert_before(it, move_result_pseudo);

        insn->set_src(index++, new_regs_size);
        new_regs_size += is_wide_type(field->get_type()) ? 2 : 1;
      }

      insn->set_arg_word_count(new_regs_size - regs_size + 1);
      code->set_registers_size(new_regs_size);
    }
  }

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
    } else {
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
  } else if (is_move_result(op)) {
    regs[insn->dest()] = regs[regs_size];
  } else if (writes_result_register(op)) {
    if (is_invoke(op)) {
      auto invoked = insn->get_method();
      auto def = resolve_method(invoked, MethodSearch::Any);
      if (def) {
        invoked = def;
      }

      if (invoked->get_proto()->get_rtype() == obj) {
        regs[regs_size] = 1;
        return;
      }
    }
    regs[regs_size] = 0;
  } else if (insn->dests_size() != 0) {
    regs[insn->dest()] = 0;
  }
}

bool tainted_reg_escapes(
    DexType* ty,
    DexMethod* method,
    const std::unordered_map<IRInstruction*, TaintedRegs>& taint_map,
    bool enable_buildee_constr_change) {
  always_assert(ty != nullptr);

  for (auto it : taint_map) {
    auto insn = it.first;
    auto tainted = it.second.bits();
    auto op = insn->opcode();
    if (is_invoke(op)) {
      auto invoked = resolve_method(insn->get_method(), opcode_to_search(insn));
      size_t args_reg_start{0};
      if (invoked == nullptr) {
        TRACE(BUILDERS, 5, "Unable to resolve %s\n", SHOW(insn));
        continue;
      }

      if (is_init(invoked) ||
          (invoked->get_class() == ty && !is_invoke_static(op))) {
        // if a builder is passed as the first arg to a virtual function or a
        // ctor, we can treat it as non-escaping, since we also check that
        // those methods don't allow the builder to escape.
        //
        // TODO: we should be able to relax the check above to be simply
        // `!is_static(invoked)`. We don't even need to check that the type
        // matches -- if the builder is being passed as the first arg reg
        // to a non-static function, it must be the `this` arg. And if the
        // non-static function is part of a different class hierarchy, the
        // builder cannot possibly be passed as the `this` arg.
        args_reg_start = 1;
      }
      for (size_t i = args_reg_start; i < insn->srcs_size(); ++i) {
        if (tainted[insn->src(i)]) {

          if (enable_buildee_constr_change) {
            // Don't consider builders that get passed to the buildee's
            // constructor. `update_buildee_constructor` will sort this
            // out later.
            if (is_init(invoked) &&
                invoked->get_class() == get_buildee(ty) &&
                has_only_argument(invoked, ty)) {

              // If the 'fields constructor' already exist, don't continue.
              if (get_fields_constr_if_exists(
                    invoked, type_class(ty)) == nullptr) {
                continue;
              }
            }
          }

          TRACE(BUILDERS, 5, "Escaping instruction: %s\n", SHOW(insn));
          return true;
        }
      }
    } else if (op == OPCODE_SPUT_OBJECT || op == OPCODE_IPUT_OBJECT ||
               op == OPCODE_APUT_OBJECT || op == OPCODE_RETURN_OBJECT) {
      if (tainted[insn->src(0)]) {
        if (op == OPCODE_RETURN_OBJECT && method->get_class() == ty) {
          continue;
        }
        TRACE(BUILDERS, 5, "Escaping instruction: %s\n", SHOW(insn));
        return true;
      }
    } else if (is_conditional_branch(op) || is_monitor(op)) {
      if (tainted[insn->src(0)]) {
        // TODO(emmasevastian): Treat this case separate.
        return true;
      }
    }
  }
  return false;
}

/**
 * Keep track, per instruction, what register(s) holds
 * an instance of the `type`.
 */
std::unique_ptr<std::unordered_map<IRInstruction*, TaintedRegs>>
get_tainted_regs(uint16_t regs_size,
                 const std::vector<cfg::Block*>& blocks,
                 DexType* type) {

  std::function<void(IRList::iterator, TaintedRegs*)> trans =
      [&](IRList::iterator it, TaintedRegs* tregs) {
        auto insn = it->insn;
        auto& regs = tregs->m_reg_set;
        auto op = insn->opcode();
        if (opcode::is_move_result_pseudo(op) &&
            std::prev(it)->insn->opcode() == OPCODE_NEW_INSTANCE) {
          DexType* cls = std::prev(it)->insn->get_type();
          auto dest = it->insn->dest();
          if (cls == type) {
            regs[dest] = 1;
          } else {
            regs[dest] = 0;
          }
        } else {
          transfer_object_reach(type, regs_size, insn, tregs->m_reg_set);
        }
      };

  // The extra register is used to keep track of the return values.
  return forwards_dataflow(blocks, TaintedRegs(regs_size + 1), trans);
}

//////////////////////////////////////////////

bool has_builder_name(DexType* type) {
  always_assert(type != nullptr);

  static boost::regex re{"\\$Builder;$"};

  const auto& deobfuscated_name = type_class(type)->get_deobfuscated_name();
  if (!deobfuscated_name.empty()) {
    return boost::regex_search(deobfuscated_name.c_str(), re);
  }
  return boost::regex_search(type->c_str(), re);
}

DexType* get_buildee(DexType* builder) {
  always_assert(builder != nullptr);

  const auto& deobfuscated_name = type_class(builder)->get_deobfuscated_name();
  const auto& builder_name =
      !deobfuscated_name.empty() ? deobfuscated_name : builder->str();

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
      auto invoked = resolve_method(insn->get_method(), opcode_to_search(insn));
      if (invoked != nullptr && invoked->get_class() == type) {
        methods.emplace_back(invoked);
      }
    }
  }

  return methods;
}

std::vector<DexMethod*> get_non_init_methods(IRCode* code, DexType* type) {
  std::vector<DexMethod*> methods = get_all_methods(code, type);
  methods.erase(remove_if(methods.begin(),
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

    for (const auto& inlinable : to_inline) {
      if (!inlinable->get_code()) {
        TRACE(BUILDERS,
              2,
              "Trying to inline abstract / native etc method: %s in %s\n",
              SHOW(inlinable),
              SHOW(method));
        return false;
      }
    }

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
                         BuilderTransform& b_transform,
                         DexType* super_class_holder) {
  DexType* buildee = get_buildee(builder->get_type());
  always_assert(buildee != nullptr);

  DexType* super_class = super_class_holder != nullptr
                             ? super_class_holder
                             : builder->get_super_class();

  // TODO(emmasevastian): extend it.
  static DexType* object_type = get_object_type();
  if (super_class != object_type) {
    return false;
  }

  bool tried_constructor_inlining = false;
  while (get_non_trivial_init_methods(method->get_code(), builder->get_type())
                 .size() > 0) {
    tried_constructor_inlining = true;

    // Filter out builders for which the method contains super class invokes.
    if (has_super_class_initializations(method, super_class)) {
      return false;
    }

    if (!b_transform.inline_methods(
            method, builder->get_type(), &get_non_trivial_init_methods) ||
        !b_transform.inline_methods(
            method, builder->get_type(), &get_non_init_methods)) {
      return false;
    }
  }

  if (!update_buildee_constructor(method, builder)) {
    return false;
  }

  if (!remove_builder(method, builder)) {
    return false;
  }

  // Cleanup after constructor inlining.
  if (tried_constructor_inlining) {
    remove_super_class_calls(method, super_class);
  }
  return true;
}
