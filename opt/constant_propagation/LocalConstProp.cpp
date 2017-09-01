/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "LocalConstProp.h"

#include <boost/optional.hpp>
#include <cmath>
#include <stack>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexOpcode.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InstructionSelection.h"
#include "Walkers.h"

/** Local (basic block level) constant propagation.
 *
 * This analysis goes instruction by instruction at the basic block boundary
 * and gathers facts about constants and propagates them inside the constant
 * value lattice model defined in GlobalConstProp.h
 *
 * This alone can be used to drive a simple constant propagation analysis that
 * will reset itself after traversing a basic block and moving on to another.
 *
 * The idea is for this analysis to be composed with GlobalConstantPropagation
 * which properly combines different facts about constants at the cross basic
 * block level.
 */

namespace {

template <typename Integral>
bool get_constant_value(const ConstPropEnvironment& env,
                        int16_t reg,
                        Integral& result);

template <>
bool get_constant_value(const ConstPropEnvironment& env,
                        int16_t reg,
                        int64_t& result) {
  if (ConstPropEnvUtil::is_wide_constant(env, reg)) {
    result = ConstPropEnvUtil::get_wide(env, reg);
    return true;
  } else {
    return false;
  }
}

template <>
bool get_constant_value(const ConstPropEnvironment& env,
                        int16_t reg,
                        int32_t& result) {
  if (ConstPropEnvUtil::is_narrow_constant(env, reg)) {
    result = ConstPropEnvUtil::get_narrow(env, reg);
    return true;
  } else {
    return false;
  }
}

template <typename Out, typename In>
// reinterpret the long's bits as a double
static Out reinterpret_bits(In in) {
  if (std::is_same<In, Out>::value) {
    return in;
  }
  static_assert(sizeof(In) == sizeof(Out), "types must be same size");
  return *reinterpret_cast<Out*>(&in);
}

bool is_compare_floating(DexOpcode op) {
  return op == OPCODE_CMPG_DOUBLE || op == OPCODE_CMPL_DOUBLE ||
         op == OPCODE_CMPG_FLOAT || op == OPCODE_CMPL_FLOAT;
}

bool is_less_than_bias(DexOpcode op) {
  return op == OPCODE_CMPL_DOUBLE || op == OPCODE_CMPL_FLOAT;
}

// Propagate the result of a compare if the operands are known constants.
// If we know enough, put -1, 0, or 1 into the destination register.
//
// Stored is how the data is stored in the register (the size).
//   Should be int32_t or int64_t
// Operand is how the data is used.
//   Should be float, double, or int64_t
template <typename Operand, typename Stored>
void analyze_compare(const IRInstruction* inst,
                     ConstPropEnvironment* current_state) {
  DexOpcode op = inst->opcode();
  Stored left_value;
  Stored right_value;
  auto left = get_constant_value(*current_state, inst->src(0), left_value);
  auto right = get_constant_value(*current_state, inst->src(1), right_value);

  if (left && right) {
    int32_t result;
    auto l_val = reinterpret_bits<Operand, Stored>(left_value);
    auto r_val = reinterpret_bits<Operand, Stored>(right_value);
    if (is_compare_floating(op) && (std::isnan(l_val) || std::isnan(r_val))) {
      if (is_less_than_bias(op)) {
        result = -1;
      } else {
        result = 1;
      }
    } else if (l_val > r_val) {
      result = 1;
    } else if (l_val == r_val) {
      result = 0;
    } else { // l_val < r_val
      result = -1;
    }
    TRACE(CONSTP,
          5,
          "Propagated constant in branch instruction %s, "
          "Operands [%d] [%d] -> Result: [%d]\n",
          SHOW(inst),
          l_val,
          r_val,
          result);
    ConstPropEnvUtil::set_narrow(*current_state, inst->dest(), result);
  } else {
    ConstPropEnvUtil::set_top(*current_state, inst->dest());
  }
}

} // namespace anonymous

void LocalConstantPropagation::analyze_instruction(
    IRInstruction* const& inst, ConstPropEnvironment* current_state) {
  TRACE(CONSTP, 5, "Analyzing instruction: %s\n", SHOW(inst));
  switch (inst->opcode()) {
  case OPCODE_CONST:
  case OPCODE_CONST_HIGH16:
  case OPCODE_CONST_4:
  case OPCODE_CONST_16: {
    TRACE(CONSTP,
          5,
          "Discovered new narrow constant for reg: %d, value: %d\n",
          inst->dest(),
          inst->literal());
    ConstPropEnvUtil::set_narrow(*current_state, inst->dest(), inst->literal());
    break;
  }
  case OPCODE_CONST_WIDE_16:
  case OPCODE_CONST_WIDE_32:
  case OPCODE_CONST_WIDE:
  case OPCODE_CONST_WIDE_HIGH16: {
    TRACE(CONSTP,
          5,
          "Discovered new wide constant for regs: %d, %d, value: %ld\n",
          inst->dest(),
          inst->dest() + 1,
          inst->literal());
    ConstPropEnvUtil::set_wide(*current_state, inst->dest(), inst->literal());
    break;
  }
  case OPCODE_MOVE_FROM16:
  case OPCODE_MOVE_16:
  case OPCODE_MOVE_OBJECT_FROM16:
  case OPCODE_MOVE_OBJECT_16:
  case OPCODE_MOVE:
  case OPCODE_MOVE_OBJECT: {
    analyze_move(inst, current_state, false /* is_wide */);
    break;
  }
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_WIDE_FROM16:
  case OPCODE_MOVE_WIDE_16: {
    analyze_move(inst, current_state, true /* is_wide */);
    break;
  }

  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT: {
    analyze_compare<float, int32_t>(inst, current_state);
    break;
  }

  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE: {
    analyze_compare<double, int64_t>(inst, current_state);
    break;
  }

  case OPCODE_CMP_LONG: {
    analyze_compare<int64_t, int64_t>(inst, current_state);
    break;
  }

  default: {
    if (inst->dests_size()) {
      TRACE(CONSTP,
            5,
            "Marking value unknown [Reg: %d, Is wide: %d]\n",
            inst->dest(),
            inst->dest_is_wide());
      ConstPropEnvUtil::set_top(
          *current_state, inst->dest(), inst->dest_is_wide());
    }
  }
  }
}

void LocalConstantPropagation::analyze_move(IRInstruction* const& inst,
                                            ConstPropEnvironment* current_state,
                                            bool is_wide) {
  auto src = inst->src(0);
  auto dst = inst->dest();
  if (!is_wide && ConstPropEnvUtil::is_narrow_constant(*current_state, src)) {
    auto value = ConstPropEnvUtil::get_narrow(*current_state, src);
    TRACE(CONSTP,
          5,
          "Propagating narrow constant [Reg: %d, Value: %X] -> "
          "[Reg: %d]\n",
          src,
          value,
          dst);
    ConstPropEnvUtil::set_narrow(*current_state, dst, value);
  } else if (is_wide &&
             ConstPropEnvUtil::is_wide_constant(*current_state, src)) {
    auto value = ConstPropEnvUtil::get_wide(*current_state, src);
    TRACE(CONSTP,
          5,
          "Propagating wide constant [Reg: %d, Value: %lX] -> "
          "[Reg: %d]\n",
          src,
          value,
          dst);
    ConstPropEnvUtil::set_wide(*current_state, dst, value);
  } else {
    TRACE(CONSTP,
          5,
          "Marking value unknown [Reg: %d, Is wide: %d]\n",
          dst,
          is_wide);
    ConstPropEnvUtil::set_top(*current_state, dst, is_wide);
  }
}

// Evaluate the guard expression of an if opcode.
// pass 0 as the r_val for if-*Z opcodes
bool eval_if(DexOpcode op, int32_t l_val, int32_t r_val) {
  switch (op) {
  case OPCODE_IF_EQ:
  case OPCODE_IF_EQZ:
    return l_val == r_val;
  case OPCODE_IF_NE:
  case OPCODE_IF_NEZ:
    return l_val != r_val;
  case OPCODE_IF_LT:
  case OPCODE_IF_LTZ:
    return l_val < r_val;
  case OPCODE_IF_GE:
  case OPCODE_IF_GEZ:
    return l_val >= r_val;
  case OPCODE_IF_GT:
  case OPCODE_IF_GTZ:
    return l_val > r_val;
  case OPCODE_IF_LE:
  case OPCODE_IF_LEZ:
    return l_val <= r_val;
  default:
    always_assert_log(false, "opcode %s must be an if", SHOW(op));
  }
}

// If we can prove the operands of a branch instruction are constant values
// replace the conditional branch with an unconditional one.
void LocalConstantPropagation::simplify_branch(
    IRInstruction*& inst, const ConstPropEnvironment& current_state) {
  int32_t left_value;
  int32_t right_value;

  auto left = get_constant_value(current_state, inst->src(0), left_value);
  if (!left) {
    return;
  }

  if (inst->srcs_size() == 2) {
    auto right = get_constant_value(current_state, inst->src(1), right_value);
    if (!right) {
      return;
    }
  } else {
    // if-*Z vA        is the same as
    // if-*  vA, 0
    right_value = 0;
  }

  bool branch_result = eval_if(inst->opcode(), left_value, right_value);
  TRACE(CONSTP, 2, "Changed conditional branch %s\n", SHOW(inst));

  IRInstruction* replacement = nullptr;
  if (branch_result) {
    // Transform keeps track of the target and selects the right size
    // instruction based on the offset
    replacement = new IRInstruction(OPCODE_GOTO);
  } else {
    replacement = new IRInstruction(OPCODE_NOP);
  }

  ++m_branch_propagated;
  m_insn_replacements.emplace_back(inst, replacement);
}

void LocalConstantPropagation::simplify_instruction(
    IRInstruction*& inst, const ConstPropEnvironment& current_state) {
  switch (inst->opcode()) {
  case OPCODE_MOVE:
  case OPCODE_MOVE_FROM16:
  case OPCODE_MOVE_16:
    if (m_config.replace_moves_with_consts) {
      simplify_move(inst, current_state, false /* is_wide */);
    }
    break;
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_WIDE_FROM16:
  case OPCODE_MOVE_WIDE_16:
    if (m_config.replace_moves_with_consts) {
      simplify_move(inst, current_state, true /* is_wide */);
    }
    break;
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ: {
    simplify_branch(inst, current_state);
    break;
  }

  default: {}
  }
}

void LocalConstantPropagation::simplify_move(
    IRInstruction* const& inst,
    const ConstPropEnvironment& current_state,
    bool is_wide) {
  uint16_t src = inst->src(0);
  uint16_t dst = inst->dest();

  uint64_t value;
  IRInstruction* replacement = nullptr;
  if (!is_wide && ConstPropEnvUtil::is_narrow_constant(current_state, src)) {
    value = ConstPropEnvUtil::get_narrow(current_state, src);
    replacement = new IRInstruction(OPCODE_CONST);
  } else if (is_wide &&
             ConstPropEnvUtil::is_wide_constant(current_state, src)) {
    value = ConstPropEnvUtil::get_wide(current_state, src);
    replacement = new IRInstruction(OPCODE_CONST_WIDE);
  } else {
    return;
  }

  replacement->set_literal(value);
  replacement->set_dest(dst);
  m_insn_replacements.emplace_back(inst, replacement);
  ++m_move_to_const;
}
