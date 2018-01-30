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

 // Note: MSVC STL didn't implement std::isnan(Integral arg). We need to provide
 // an override of fpclassify for integral types.
#ifdef _MSC_VER
#include <type_traits>
template <typename T>
std::enable_if_t<std::is_integral<T>::value, int> fpclassify(T x) {
  return x == 0 ? FP_ZERO : FP_NORMAL;
}
#endif

#include <cmath>
#include <functional>
#include <limits>
#include <stack>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "InstructionLowering.h"
#include "Resolver.h"
#include "Walkers.h"

/** Local (basic block level) constant propagation.
 *
 * This analysis goes instruction by instruction at the basic block boundary
 * and gathers facts about constants and propagates them inside the constant
 * value lattice model defined in ConstantEnvironment.h
 *
 * This alone can be used to drive a simple constant propagation analysis that
 * will reset itself after traversing a basic block and moving on to another.
 *
 * The idea is for this analysis to be composed with GlobalConstantPropagation
 * which properly combines different facts about constants at the cross basic
 * block level.
 */

namespace {

template <typename Out, typename In>
// reinterpret the long's bits as a double
static Out reinterpret_bits(In in) {
  if (std::is_same<In, Out>::value) {
    return in;
  }
  static_assert(sizeof(In) == sizeof(Out), "types must be same size");
  return *reinterpret_cast<Out*>(&in);
}

bool is_compare_floating(IROpcode op) {
  return op == OPCODE_CMPG_DOUBLE || op == OPCODE_CMPL_DOUBLE ||
         op == OPCODE_CMPG_FLOAT || op == OPCODE_CMPL_FLOAT;
}

bool is_less_than_bias(IROpcode op) {
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
                     ConstantEnvironment* current_state) {
  IROpcode op = inst->opcode();
  auto left = current_state->get(inst->src(0)).constant_domain().get_constant();
  auto right =
      current_state->get(inst->src(1)).constant_domain().get_constant();

  if (left && right) {
    int32_t result;
    auto l_val = reinterpret_bits<Operand, Stored>(*left);
    auto r_val = reinterpret_bits<Operand, Stored>(*right);
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
    current_state->set(inst->dest(), SignedConstantDomain(result));
  } else {
    current_state->set(inst->dest(), SignedConstantDomain::top());
  }
}

} // namespace

bool addition_out_of_bounds(int64_t a, int64_t b) {
  int32_t max = std::numeric_limits<int32_t>::max();
  int32_t min = std::numeric_limits<int32_t>::min();
  if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) {
    TRACE(CONSTP, 5, "%d, %d is out of bounds", a, b);
    return true;
  }
  return false;
}

void LocalConstantPropagation::analyze_instruction(
    const IRInstruction* insn, ConstantEnvironment* current_state) {
  TRACE(CONSTP, 5, "Analyzing instruction: %s\n", SHOW(insn));
  switch (insn->opcode()) {
  case OPCODE_CONST:
  case OPCODE_CONST_WIDE: {
    TRACE(CONSTP,
          5,
          "Discovered new constant for reg: %d value: %ld\n",
          insn->dest(),
          insn->get_literal());
    current_state->set(insn->dest(), SignedConstantDomain(insn->get_literal()));
    break;
  }
  case OPCODE_MOVE:
  case OPCODE_MOVE_OBJECT: {
    analyze_non_branch(insn, current_state);
    break;
  }
  case OPCODE_MOVE_WIDE: {
    analyze_non_branch(insn, current_state);
    break;
  }

  case OPCODE_MOVE_RESULT:
  case OPCODE_MOVE_RESULT_WIDE:
  case OPCODE_MOVE_RESULT_OBJECT:
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    current_state->set(insn->dest(), current_state->get(RESULT_REGISTER));
    break;

  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT: {
    analyze_compare<float, int32_t>(insn, current_state);
    break;
  }

  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE: {
    analyze_compare<double, int64_t>(insn, current_state);
    break;
  }

  case OPCODE_CMP_LONG: {
    analyze_compare<int64_t, int64_t>(insn, current_state);
    break;
  }

  case OPCODE_SGET:
  case OPCODE_SGET_WIDE:
  case OPCODE_SGET_OBJECT:
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT: {
    auto field = resolve_field(insn->get_field());
    current_state->set(RESULT_REGISTER, m_field_env.get(field));
    break;
  }

  case OPCODE_ADD_INT_LIT16:
  case OPCODE_ADD_INT_LIT8: {
    // add-int/lit8 is the most common arithmetic instruction: about .29% of
    // all instructions. All other arithmetic instructions are less than .05%
    if (m_config.fold_arithmetic) {
      int32_t lit = insn->get_literal();
      auto add_in_bounds = [lit](int64_t v) -> boost::optional<int64_t> {
        if (addition_out_of_bounds(lit, v)) {
          return boost::none;
        }
        return v + lit;
      };
      TRACE(CONSTP,
            5,
            "Attempting to fold %s with literal %lu\n",
            SHOW(insn),
            lit);
      analyze_non_branch(insn, current_state, add_in_bounds);
      break;
    }
    // fallthrough
  }

  default: {
    if (insn->dests_size()) {
      TRACE(CONSTP, 5, "Marking value unknown [Reg: %d]\n", insn->dest());
      current_state->set(insn->dest(), SignedConstantDomain::top());
    } else if (insn->has_move_result() || insn->has_move_result_pseudo()) {
      TRACE(CONSTP, 5, "Clearing result register\n");
      current_state->set(RESULT_REGISTER, SignedConstantDomain::top());
    }
  }
  }
}

// We can use this function for moves and arithmetic instructions because a move
// is just an arithmetic instruction with identity as its transform function
void LocalConstantPropagation::analyze_non_branch(
    const IRInstruction* inst,
    ConstantEnvironment* current_state,
    value_transform_t value_transform // default is identity
) {
  auto src = inst->src(0);
  auto dst = inst->dest();

  auto cst = current_state->get(src).constant_domain().get_constant();
  auto value = cst ? value_transform(*cst) : boost::none;
  if (!value) {
    TRACE(CONSTP, 5, "Marking value unknown [Reg: %d]\n", dst);
    current_state->set(dst, SignedConstantDomain::top());
    return;
  }
  TRACE(CONSTP,
        5,
        "Propagating constant [Value: %X] -> [Reg: %d]\n",
        src,
        *value,
        dst);
  current_state->set(dst, SignedConstantDomain(*value));
}

// Evaluate the guard expression of an if opcode. Return boost::none if the
// branch cannot be determined to jump the same way every time. Otherwise
// return true if the branch is always taken and false if it is never taken.
boost::optional<bool> eval_if(IRInstruction*& insn,
                              const ConstantEnvironment& state) {
  if (state.is_bottom()) {
    return boost::none;
  }
  auto op = insn->opcode();
  auto scd_left = state.get(insn->src(0));
  auto scd_right =
      insn->srcs_size() > 1 ? state.get(insn->src(1)) : SignedConstantDomain(0);
  switch (op) {
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ: {
    auto cd_left = scd_left.constant_domain();
    auto cd_right = scd_right.constant_domain();
    if (!(cd_left.is_value() && cd_right.is_value())) {
      return boost::none;
    }
    if (op == OPCODE_IF_EQ || op == OPCODE_IF_EQZ) {
      return *cd_left.get_constant() == *cd_right.get_constant();
    } else { // IF_NE / IF_NEZ
      return *cd_left.get_constant() != *cd_right.get_constant();
    }
  }
  case OPCODE_IF_LE:
  case OPCODE_IF_LEZ: {
    if (scd_left.max_element() <= scd_right.min_element()) {
      return true;
    } else if (scd_left.min_element() > scd_right.max_element()) {
      return false;
    } else {
      return boost::none;
    }
  }
  case OPCODE_IF_LT:
  case OPCODE_IF_LTZ: {
    if (scd_left.max_element() < scd_right.min_element()) {
      return true;
    } else if (scd_left.min_element() >= scd_right.max_element()) {
      return false;
    } else {
      return boost::none;
    }
  }
  case OPCODE_IF_GE:
  case OPCODE_IF_GEZ: {
    if (scd_left.min_element() >= scd_right.max_element()) {
      return true;
    } else if (scd_left.max_element() < scd_right.min_element()) {
      return false;
    } else {
      return boost::none;
    }
  }
  case OPCODE_IF_GT:
  case OPCODE_IF_GTZ: {
    if (scd_left.min_element() > scd_right.max_element()) {
      return true;
    } else if (scd_left.max_element() <= scd_right.min_element()) {
      return false;
    } else {
      return boost::none;
    }
  }
  default:
    always_assert_log(false, "opcode %s must be an if", SHOW(op));
  }
}

// If we can prove the operands of a branch instruction are constant values
// replace the conditional branch with an unconditional one.
void LocalConstantPropagation::simplify_branch(
    IRInstruction*& inst, const ConstantEnvironment& current_state) {
  auto constant_branch = eval_if(inst, current_state);

  if (!constant_branch) {
    return;
  }
  TRACE(CONSTP,
        2,
        "Changed conditional branch %s as it is always %s\n",
        SHOW(inst),
        *constant_branch ? "true" : "false");
  ++m_branch_propagated;
  // Transform keeps track of the target and selects the right size
  // instruction based on the offset
  m_insn_replacements.emplace_back(
      inst, new IRInstruction(*constant_branch ? OPCODE_GOTO : OPCODE_NOP));
}

void LocalConstantPropagation::simplify_instruction(
    IRInstruction*& inst, const ConstantEnvironment& current_state) {
  switch (inst->opcode()) {
  case OPCODE_MOVE:
    if (m_config.replace_moves_with_consts) {
      simplify_non_branch(inst, current_state, false /* is_wide */);
    }
    break;
  case OPCODE_MOVE_WIDE:
    if (m_config.replace_moves_with_consts) {
      simplify_non_branch(inst, current_state, true /* is_wide */);
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

  case OPCODE_ADD_INT_LIT16:
  case OPCODE_ADD_INT_LIT8: {
    if (m_config.fold_arithmetic) {
      simplify_non_branch(inst, current_state, false);
    }
    break;
  }

  default: {}
  }
}

// Replace an instruction that has a single destination register with a `const`
// load. `current_state` holds the state of the registers after `inst` has been
// evaluated. So, `current_state[dst]` holds the _new_ value of the destination
// register
void LocalConstantPropagation::simplify_non_branch(
    IRInstruction* inst,
    const ConstantEnvironment& current_state,
    bool is_wide) {
  // we read from `dest` because analyze has put the new value of dst there
  auto cst = current_state.get(inst->dest()).constant_domain().get_constant();
  if (!cst) {
    return;
  }
  IRInstruction* replacement =
      new IRInstruction(is_wide ? OPCODE_CONST_WIDE : OPCODE_CONST);
  replacement->set_literal(*cst);
  replacement->set_dest(inst->dest());

  TRACE(CONSTP, 5, "Replacing %s with %s\n", SHOW(inst), SHOW(replacement));
  m_insn_replacements.emplace_back(inst, replacement);
  ++m_materialized_consts;
}
