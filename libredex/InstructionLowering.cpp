/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "InstructionLowering.h"
#include "Walkers.h"

#include "boost/algorithm/string/join.hpp"
#include "boost/range/adaptors.hpp"

namespace instruction_lowering {

/*
 * Returns whether the given value can fit in an integer of :width bits.
 */
template <int width>
static bool signed_int_fits(int64_t v) {
  auto shift = 64 - width;
  return (v << shift >> shift) == v;
}

/*
 * Returns whether the given value's significant bits can fit in the top 16
 * bits of an integer of :total_width bits. For example, since v is a signed
 * 64-bit int, a value v that can fit into the top 16 bits of a 32-bit int
 * would have the form 0xffffffffrrrr0000, where rrrr are the significant bits.
 */
template <int total_width>
static bool signed_int_fits_high16(int64_t v) {
  auto right_zeros = total_width - 16;
  auto left_ones = 64 - total_width;
  return v >> right_zeros << (64 - 16) >> left_ones == v;
}

/*
 * Helpers for lower()
 */

/*
 * Returns an array of move opcodes of the appropriate type, sorted by
 * increasing size.
 */
static std::array<DexOpcode, 3> move_opcode_tuple(IROpcode op) {
  switch (op) {
  case OPCODE_MOVE:
    return {{DOPCODE_MOVE, DOPCODE_MOVE_FROM16, DOPCODE_MOVE_16}};
  case OPCODE_MOVE_WIDE:
    return {
        {DOPCODE_MOVE_WIDE, DOPCODE_MOVE_WIDE_FROM16, DOPCODE_MOVE_WIDE_16}};
  case OPCODE_MOVE_OBJECT:
    return {{DOPCODE_MOVE_OBJECT,
             DOPCODE_MOVE_OBJECT_FROM16,
             DOPCODE_MOVE_OBJECT_16}};
  default:
    not_reached();
  }
}

namespace impl {

DexOpcode select_move_opcode(const IRInstruction* insn) {
  auto move_tuple = move_opcode_tuple(insn->opcode());
  auto dest_width = required_bit_width(insn->dest());
  auto src_width = required_bit_width(insn->src(0));
  if (dest_width <= 4 && src_width <= 4) {
    return move_tuple.at(0);
  } else if (dest_width <= 8) {
    return move_tuple.at(1);
  } else {
    return move_tuple.at(2);
  }
}

DexOpcode select_const_opcode(const IRInstruction* insn) {
  auto op = insn->opcode();
  auto dest_width = required_bit_width(insn->dest());
  always_assert(dest_width <= 8);
  auto literal = insn->get_literal();
  switch (op) {
  case OPCODE_CONST:
    if (dest_width <= 4 && signed_int_fits<4>(literal)) {
      return DOPCODE_CONST_4;
    } else if (signed_int_fits<16>(literal)) {
      return DOPCODE_CONST_16;
    } else if (signed_int_fits_high16<32>(literal)) {
      return DOPCODE_CONST_HIGH16;
    } else {
      always_assert(signed_int_fits<32>(literal));
      return DOPCODE_CONST;
    }
  case OPCODE_CONST_WIDE:
    if (signed_int_fits<16>(literal)) {
      return DOPCODE_CONST_WIDE_16;
    } else if (signed_int_fits<32>(literal)) {
      return DOPCODE_CONST_WIDE_32;
    } else if (signed_int_fits_high16<64>(literal)) {
      return DOPCODE_CONST_WIDE_HIGH16;
    } else {
      return DOPCODE_CONST_WIDE;
    }
  default:
    not_reached();
  }
}

DexOpcode select_binop_lit_opcode(const IRInstruction* insn) {
  auto op = insn->opcode();
  auto literal = insn->get_literal();
  if (signed_int_fits<8>(literal)) { // lit8 -> literal is 8 bits
    switch (op) {
    case OPCODE_ADD_INT_LIT8:
    case OPCODE_ADD_INT_LIT16:
      return DOPCODE_ADD_INT_LIT8;
    case OPCODE_RSUB_INT_LIT8:
    case OPCODE_RSUB_INT:
      return DOPCODE_RSUB_INT_LIT8;
    case OPCODE_MUL_INT_LIT8:
    case OPCODE_MUL_INT_LIT16:
      return DOPCODE_MUL_INT_LIT8;
    case OPCODE_DIV_INT_LIT8:
    case OPCODE_DIV_INT_LIT16:
      return DOPCODE_DIV_INT_LIT8;
    case OPCODE_REM_INT_LIT8:
    case OPCODE_REM_INT_LIT16:
      return DOPCODE_REM_INT_LIT8;
    case OPCODE_AND_INT_LIT8:
    case OPCODE_AND_INT_LIT16:
      return DOPCODE_AND_INT_LIT8;
    case OPCODE_OR_INT_LIT8:
    case OPCODE_OR_INT_LIT16:
      return DOPCODE_OR_INT_LIT8;
    case OPCODE_XOR_INT_LIT8:
    case OPCODE_XOR_INT_LIT16:
      return DOPCODE_XOR_INT_LIT8;
    case OPCODE_SHL_INT_LIT8:
      return DOPCODE_SHL_INT_LIT8;
    case OPCODE_SHR_INT_LIT8:
      return DOPCODE_SHR_INT_LIT8;
    case OPCODE_USHR_INT_LIT8:
      return DOPCODE_USHR_INT_LIT8;
    default:
      not_reached();
    }
  } else if (signed_int_fits<16>(literal)) { // lit16 -> literal is 16 bits
    switch (op) {
    case OPCODE_ADD_INT_LIT8:
    case OPCODE_ADD_INT_LIT16:
      return DOPCODE_ADD_INT_LIT16;
    case OPCODE_RSUB_INT_LIT8:
    case OPCODE_RSUB_INT:
      return DOPCODE_RSUB_INT;
    case OPCODE_MUL_INT_LIT8:
    case OPCODE_MUL_INT_LIT16:
      return DOPCODE_MUL_INT_LIT16;
    case OPCODE_DIV_INT_LIT8:
    case OPCODE_DIV_INT_LIT16:
      return DOPCODE_DIV_INT_LIT16;
    case OPCODE_REM_INT_LIT8:
    case OPCODE_REM_INT_LIT16:
      return DOPCODE_REM_INT_LIT16;
    case OPCODE_AND_INT_LIT8:
    case OPCODE_AND_INT_LIT16:
      return DOPCODE_AND_INT_LIT16;
    case OPCODE_OR_INT_LIT8:
    case OPCODE_OR_INT_LIT16:
      return DOPCODE_OR_INT_LIT16;
    case OPCODE_XOR_INT_LIT8:
    case OPCODE_XOR_INT_LIT16:
      return DOPCODE_XOR_INT_LIT16;
    default:
      not_reached();
    }
  } else {
    // literal > 16 not yet supported
    always_assert_log(
        false, "binop_lit doesn't support literals greater than 16 bits");
  }
}

bool try_2addr_conversion(MethodItemEntry* mie) {
  auto* insn = mie->dex_insn;
  auto op = insn->opcode();
  if (dex_opcode::is_commutative(op) && insn->dest() == insn->src(1) &&
      insn->dest() <= 0xf && insn->src(0) <= 0xf) {
    auto* new_insn = new DexInstruction(convert_3to2addr(op));
    new_insn->set_dest(insn->dest());
    new_insn->set_src(1, insn->src(0));
    delete mie->dex_insn;
    mie->dex_insn = new_insn;
    return true;
  } else if (op >= DOPCODE_ADD_INT && op <= DOPCODE_REM_DOUBLE &&
             insn->dest() == insn->src(0) && insn->dest() <= 0xf &&
             insn->src(1) <= 0xf) {
    auto* new_insn = new DexInstruction(convert_3to2addr(op));
    new_insn->set_dest(insn->dest());
    new_insn->set_src(1, insn->src(1));
    delete mie->dex_insn;
    mie->dex_insn = new_insn;
    return true;
  }
  return false;
}

} // namespace impl

using namespace impl;

/*
 * Checks that the load-param opcodes are consistent with the method prototype.
 */
static void check_load_params(DexMethod* method) {
  auto* code = method->get_code();
  auto params = code->get_param_instructions();
  auto param_ops = InstructionIterable(params);
  if (param_ops.empty()) {
    return;
  }
  auto& args_list = method->get_proto()->get_args()->get_type_list();
  auto it = param_ops.begin();
  auto end = param_ops.end();
  uint16_t next_ins = it->insn->dest();
  if (!is_static(method)) {
    auto op = it->insn->opcode();
    always_assert(op == IOPCODE_LOAD_PARAM_OBJECT);
    it.reset(code->erase(it.unwrap()));
    ++next_ins;
  }
  auto args_it = args_list.begin();
  for (; it != end; ++it) {
    auto op = it->insn->opcode();
    // check that the param registers are contiguous
    always_assert(next_ins == it->insn->dest());
    // TODO: have load param opcodes store the actual type of the param and
    // check that they match the method prototype here
    always_assert(args_it != args_list.end());
    if (is_wide_type(*args_it)) {
      always_assert(op == IOPCODE_LOAD_PARAM_WIDE);
    } else if (is_primitive(*args_it)) {
      always_assert(op == IOPCODE_LOAD_PARAM);
    } else {
      always_assert(op == IOPCODE_LOAD_PARAM_OBJECT);
    }
    ++args_it;
    next_ins += it->insn->dest_is_wide() ? 2 : 1;
  }
  always_assert(args_it == args_list.end());
  // check that the params are at the end of the frame
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->dests_size()) {
      always_assert_log(insn->dest() < next_ins,
                        "Instruction %s refers to a register (v%u) >= size (%u)"
                        "in method %s\n",
                        SHOW(insn),
                        insn->dest(),
                        next_ins,
                        SHOW(method));
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      always_assert_log(insn->src(i) < next_ins,
                        "Instruction %s refers to a register (v%u) >= size (%u)"
                        "in method %s\n",
                        SHOW(insn),
                        insn->src(i),
                        next_ins,
                        SHOW(method));
    }
  }
}

static DexInstruction* create_dex_instruction(const IRInstruction* insn) {
  auto op = opcode::to_dex_opcode(insn->opcode());
  switch (opcode::ref(insn->opcode())) {
  case opcode::Ref::None:
  case opcode::Ref::Data:
    return new DexInstruction(op);
  case opcode::Ref::Literal:
    return new DexInstruction(op);
  case opcode::Ref::String:
    return new DexOpcodeString(op, insn->get_string());
  case opcode::Ref::Type:
    return new DexOpcodeType(op, insn->get_type());
  case opcode::Ref::Field:
    return new DexOpcodeField(op, insn->get_field());
  case opcode::Ref::Method:
    return new DexOpcodeMethod(op, insn->get_method());
  }
}

// IRCode::remove_opcode doesn't support removal of move-result-pseudo
// instructions in isolation -- it only removes them when the caller calls it
// with the associated 'primary' prefix instruction -- so we use this function
// specifically for this purpose.
static void remove_move_result_pseudo(IRList::iterator it) {
  always_assert(opcode::is_move_result_pseudo(it->insn->opcode()));
  delete it->insn;
  it->insn = nullptr;
  it->type = MFLOW_FALLTHROUGH;
}

/*
 * Returns the number of DexInstructions added during lowering (not including
 * the check-cast).
 */
static size_t lower_check_cast(DexMethod*,
                               IRCode* code,
                               IRList::iterator* it_) {
  auto& it = *it_;
  const auto* insn = it->insn;
  size_t extra_instructions{0};
  auto move = ir_list::move_result_pseudo_of(it);
  if (move->dest() != insn->src(0)) {
    // convert check-cast v1; move-result-pseudo v0 into
    //
    //   move v0, v1
    //   check-cast v0
    // TODO: factor this code a little
    auto move_template = std::make_unique<IRInstruction>(OPCODE_MOVE_OBJECT);
    move_template->set_dest(move->dest());
    move_template->set_src(0, insn->src(0));
    auto* dex_mov = new DexInstruction(select_move_opcode(move_template.get()));
    dex_mov->set_dest(move->dest());
    dex_mov->set_src(0, insn->src(0));
    code->insert_before(it, dex_mov);
    ++extra_instructions;
  }
  auto* dex_insn = new DexOpcodeType(DOPCODE_CHECK_CAST, insn->get_type());
  dex_insn->set_src(0, move->dest());
  it->replace_ir_with_dex(dex_insn);
  remove_move_result_pseudo(++it);

  return extra_instructions;
}

static void lower_fill_array_data(DexMethod*,
                                  IRCode* code,
                                  IRList::iterator it) {
  const auto* insn = it->insn;
  auto* dex_insn = new DexInstruction(DOPCODE_FILL_ARRAY_DATA);
  dex_insn->set_src(0, insn->src(0));
  auto* bt = new BranchTarget(&*it);
  code->push_back(bt);
  code->push_back(insn->get_data());
  it->replace_ir_with_dex(dex_insn);
}

static void lower_to_range_instruction(DexMethod* method,
                                       IRCode* code,
                                       IRList::iterator* it_) {
  using boost::adaptors::transformed;
  using boost::algorithm::join;
  auto& it = *it_;
  const auto* insn = it->insn;
  always_assert_log(
      has_contiguous_srcs(insn),
      "Instruction %s has non-contiguous srcs (%s) in method "
      "%s.\nContext:\n%s\n",
      SHOW(insn),
      join(insn->srcs() | transformed((std::string(*)(int))std::to_string),
           ", ")
          .c_str(),
      SHOW(method),
      SHOW_CONTEXT(code, insn));
  auto* dex_insn = create_dex_instruction(insn);
  dex_insn->set_opcode(opcode::range_version(insn->opcode()));
  dex_insn->set_range_base(insn->src(0));
  dex_insn->set_range_size(insn->srcs_size());
  it->replace_ir_with_dex(dex_insn);
}

static void lower_simple_instruction(DexMethod*,
                                     IRCode*,
                                     IRList::iterator* it_) {
  auto& it = *it_;
  const auto* insn = it->insn;
  auto op = insn->opcode();

  DexInstruction* dex_insn;
  if (is_move(op)) {
    dex_insn = new DexInstruction(select_move_opcode(insn));
  } else if (op >= OPCODE_CONST && op <= OPCODE_CONST_WIDE) {
    dex_insn = new DexInstruction(select_const_opcode(insn));
  } else if (op >= OPCODE_ADD_INT_LIT16 && op <= OPCODE_USHR_INT_LIT8) {
    dex_insn = new DexInstruction(select_binop_lit_opcode(insn));
  } else {
    dex_insn = create_dex_instruction(insn);
  }
  if (insn->dests_size()) {
    dex_insn->set_dest(insn->dest());
  } else if (insn->has_move_result_pseudo()) {
    dex_insn->set_dest(ir_list::move_result_pseudo_of(it)->dest());
  }
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    dex_insn->set_src(i, insn->src(i));
  }
  if (insn->has_literal()) {
    dex_insn->set_literal(insn->get_literal());
  }
  auto dex_op = dex_insn->opcode();
  if (dex_opcode::has_arg_word_count(dex_op)) {
    dex_insn->set_arg_word_count(insn->srcs_size());
  }
  it->replace_ir_with_dex(dex_insn);
  if (insn->has_move_result_pseudo()) {
    remove_move_result_pseudo(++it);
  }
}

Stats lower(DexMethod* method) {
  Stats stats;
  auto* code = method->get_code();
  always_assert(code != nullptr);
  // Check the load-param opcodes make sense before removing them
  check_load_params(method);
  for (auto it = code->begin(); it != code->end(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    auto* insn = it->insn;
    auto op = insn->opcode();
    insn->denormalize_registers();

    if (opcode::is_load_param(op)) {
      code->remove_opcode(it);
    } else if (op == OPCODE_CHECK_CAST) {
      stats.move_for_check_cast += lower_check_cast(method, code, &it);
    } else if (op == OPCODE_FILL_ARRAY_DATA) {
      lower_fill_array_data(method, code, it);
    } else if (needs_range_conversion(insn)) {
      lower_to_range_instruction(method, code, &it);
    } else {
      lower_simple_instruction(method, code, &it);
    }
  }
  for (auto it = code->begin(); it != code->end(); ++it) {
    always_assert(it->type != MFLOW_OPCODE);
    if (it->type != MFLOW_DEX_OPCODE) {
      continue;
    }
    stats.to_2addr += try_2addr_conversion(&*it);
  }
  return stats;
}

Stats run(DexStoresVector& stores) {
  using Data = std::nullptr_t;
  auto scope = build_class_scope(stores);
  return walk::parallel::reduce_methods<Data, Stats>(
      scope,
      [](Data&, DexMethod* m) {
        Stats stats;
        if (m->get_code() == nullptr) {
          return stats;
        }
        stats.accumulate(lower(m));
        return stats;
      },
      [](Stats a, Stats b) {
        a.accumulate(b);
        return a;
      },
      [&](unsigned int) { return nullptr; });
}

} // namespace instruction_lowering
