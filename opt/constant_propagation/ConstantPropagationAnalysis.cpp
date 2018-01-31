/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagationAnalysis.h"

// Note: MSVC STL doesn't implement std::isnan(Integral arg). We need to provide
// an override of fpclassify for integral types.
#ifdef _MSC_VER
#include <type_traits>
template <typename T>
std::enable_if_t<std::is_integral<T>::value, int> fpclassify(T x) {
  return x == 0 ? FP_ZERO : FP_NORMAL;
}
#endif

#include "DexUtil.h"
#include "Resolver.h"
#include "Transform.h"
#include "Walkers.h"

namespace {

/*
 * Helpers for basic block analysis
 */

// reinterpret the long's bits as a double
template <typename Out, typename In>
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

bool addition_out_of_bounds(int64_t a, int64_t b) {
  int32_t max = std::numeric_limits<int32_t>::max();
  int32_t min = std::numeric_limits<int32_t>::min();
  if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) {
    TRACE(CONSTP, 5, "%d, %d is out of bounds", a, b);
    return true;
  }
  return false;
}

using value_transform_t = std::function<boost::optional<int64_t>(int64_t)>;

void analyze_non_branch(const IRInstruction* insn,
                        ConstantEnvironment* env,
                        value_transform_t value_transform =
                            [](int64_t v) { return v; } // default is identity
) {
  auto src = insn->src(0);
  auto dst = insn->dest();

  auto cst = env->get(src).constant_domain().get_constant();
  auto value = cst ? value_transform(*cst) : boost::none;
  if (!value) {
    TRACE(CONSTP, 5, "Marking value unknown [Reg: %d]\n", dst);
    env->set(dst, SignedConstantDomain::top());
    return;
  }
  TRACE(CONSTP,
        5,
        "Propagating constant [Value: %X] -> [Reg: %d]\n",
        src,
        *value,
        dst);
  env->set(dst, SignedConstantDomain(*value));
}

// Propagate the result of a compare if the operands are known constants.
// If we know enough, put -1, 0, or 1 into the destination register.
//
// Stored is how the data is stored in the register (the size).
//   Should be int32_t or int64_t
// Operand is how the data is used.
//   Should be float, double, or int64_t
template <typename Operand, typename Stored>
void analyze_compare(const IRInstruction* insn, ConstantEnvironment* env) {
  IROpcode op = insn->opcode();
  auto left = env->get(insn->src(0)).constant_domain().get_constant();
  auto right = env->get(insn->src(1)).constant_domain().get_constant();

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
          SHOW(insn),
          l_val,
          r_val,
          result);
    env->set(insn->dest(), SignedConstantDomain(result));
  } else {
    env->set(insn->dest(), SignedConstantDomain::top());
  }
}

} // namespace

namespace constant_propagation {

namespace intraprocedural {

void FixpointIterator::analyze_instruction(const IRInstruction* insn,
                                           ConstantEnvironment* env) const {
  TRACE(CONSTP, 5, "Analyzing instruction: %s\n", SHOW(insn));
  auto op = insn->opcode();
  switch (op) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_WIDE:
  case IOPCODE_LOAD_PARAM_OBJECT:
    // We assume that the initial environment passed to run() has parameter
    // bindings already added, so do nothing here
    break;

  case OPCODE_CONST:
  case OPCODE_CONST_WIDE: {
    TRACE(CONSTP,
          5,
          "Discovered new constant for reg: %d value: %ld\n",
          insn->dest(),
          insn->get_literal());
    env->set(insn->dest(), SignedConstantDomain(insn->get_literal()));
    break;
  }
  case OPCODE_MOVE:
  case OPCODE_MOVE_OBJECT: {
    analyze_non_branch(insn, env);
    break;
  }
  case OPCODE_MOVE_WIDE: {
    analyze_non_branch(insn, env);
    break;
  }

  case OPCODE_MOVE_RESULT:
  case OPCODE_MOVE_RESULT_WIDE:
  case OPCODE_MOVE_RESULT_OBJECT:
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    env->set(insn->dest(), env->get(RESULT_REGISTER));
    break;

  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT: {
    analyze_compare<float, int32_t>(insn, env);
    break;
  }

  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE: {
    analyze_compare<double, int64_t>(insn, env);
    break;
  }

  case OPCODE_CMP_LONG: {
    analyze_compare<int64_t, int64_t>(insn, env);
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
    env->set(RESULT_REGISTER, m_field_env.get(field));
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
      analyze_non_branch(insn, env, add_in_bounds);
      break;
    }
    // fallthrough
  }

  default: {
    if (insn->dests_size()) {
      TRACE(CONSTP, 5, "Marking value unknown [Reg: %d]\n", insn->dest());
      env->set(insn->dest(), SignedConstantDomain::top());
    } else if (insn->has_move_result() || insn->has_move_result_pseudo()) {
      TRACE(CONSTP, 5, "Clearing result register\n");
      env->set(RESULT_REGISTER, SignedConstantDomain::top());
    }
  }
  }
}

void FixpointIterator::analyze_node(const NodeId& block,
                                    ConstantEnvironment* state_at_entry) const {
  TRACE(CONSTP, 5, "Analyzing block: %d\n", block->id());
  for (auto& mie : InstructionIterable(block)) {
    analyze_instruction(mie.insn, state_at_entry);
  }
}

/*
 * Helpers for CFG edge analysis
 */

/*
 * If we can determine that a branch is not taken based on the constants in the
 * environment, set the environment to bottom upon entry into the unreachable
 * block.
 */
static void analyze_if(const IRInstruction* insn,
                       ConstantEnvironment* state,
                       bool is_true_branch) {
  if (state->is_bottom()) {
    return;
  }
  // Inverting the conditional here means that we only need to consider the
  // "true" case of the if-* opcode
  auto op = !is_true_branch ? opcode::invert_conditional_branch(insn->opcode())
                            : insn->opcode();

  auto scd_left = state->get(insn->src(0));
  auto scd_right = insn->srcs_size() > 1 ? state->get(insn->src(1))
                                         : SignedConstantDomain(0);

  switch (op) {
  case OPCODE_IF_EQ: {
    auto refined_value = scd_left.meet(scd_right);
    state->set(insn->src(0), refined_value);
    state->set(insn->src(1), refined_value);
    break;
  }
  case OPCODE_IF_EQZ: {
    state->set(insn->src(0), scd_left.meet(SignedConstantDomain(0)));
    break;
  }
  case OPCODE_IF_NE:
  case OPCODE_IF_NEZ: {
    auto cd_left = scd_left.constant_domain();
    auto cd_right = scd_right.constant_domain();
    if (!(cd_left.is_value() && cd_right.is_value())) {
      break;
    }
    if (*cd_left.get_constant() == *cd_right.get_constant()) {
      state->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_LT:
    if (scd_left.min_element() >= scd_right.max_element()) {
      state->set_to_bottom();
    }
    break;
  case OPCODE_IF_LTZ:
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(sign_domain::Interval::LTZ)));
    break;
  case OPCODE_IF_GE:
    if (scd_left.max_element() < scd_right.min_element()) {
      state->set_to_bottom();
    }
    break;
  case OPCODE_IF_GEZ:
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(sign_domain::Interval::GEZ)));
    break;
  case OPCODE_IF_GT:
    if (scd_left.max_element() <= scd_right.min_element()) {
      state->set_to_bottom();
    }
    break;
  case OPCODE_IF_GTZ:
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(sign_domain::Interval::GTZ)));
    break;
  case OPCODE_IF_LE:
    if (scd_left.min_element() > scd_right.max_element()) {
      state->set_to_bottom();
    }
    break;
  case OPCODE_IF_LEZ:
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(sign_domain::Interval::LEZ)));
    break;
  default:
    always_assert_log(false, "expected if-* opcode, got %s", SHOW(insn));
    not_reached();
  }
}

ConstantEnvironment FixpointIterator::analyze_edge(
    const std::shared_ptr<cfg::Edge>& edge,
    const ConstantEnvironment& exit_state_at_source) const {
  auto env = exit_state_at_source;
  auto last_insn_it = transform::find_last_instruction(edge->src());
  if (last_insn_it == edge->src()->end()) {
    return env;
  }

  auto insn = last_insn_it->insn;
  auto op = insn->opcode();
  if (is_conditional_branch(op)) {
    analyze_if(insn, &env, edge->type() == EDGE_BRANCH);
  }
  return env;
}

} // namespace intraprocedural

} // namespace constant_propagation
