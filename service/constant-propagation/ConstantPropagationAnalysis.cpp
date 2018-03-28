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

  auto cst = env->get_primitive(src).constant_domain().get_constant();
  auto value = cst ? value_transform(*cst) : boost::none;
  if (!value) {
    TRACE(CONSTP, 5, "Marking value unknown [Reg: %d]\n", dst);
    env->set_primitive(dst, SignedConstantDomain::top());
    return;
  }
  TRACE(CONSTP, 5, "Propagating constant [Value: %X] -> [Reg: %d]\n", src,
        *value, dst);
  env->set_primitive(dst, SignedConstantDomain(*value));
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
  auto left = env->get_primitive(insn->src(0)).constant_domain().get_constant();
  auto right =
      env->get_primitive(insn->src(1)).constant_domain().get_constant();

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
    TRACE(CONSTP, 5,
          "Propagated constant in branch instruction %s, "
          "Operands [%d] [%d] -> Result: [%d]\n",
          SHOW(insn), l_val, r_val, result);
    env->set_primitive(insn->dest(), SignedConstantDomain(result));
  } else {
    env->set_primitive(insn->dest(), SignedConstantDomain::top());
  }
}

} // namespace

namespace constant_propagation {

bool LocalArraySubAnalyzer::analyze_new_array(const IRInstruction* insn,
                                              ConstantEnvironment* env) {
  auto length = env->get_primitive(insn->src(0));
  auto length_value_opt = length.constant_domain().get_constant();
  if (!length_value_opt) {
    return false;
  }
  env->set_array(RESULT_REGISTER, insn,
                 ConstantPrimitiveArrayDomain(*length_value_opt));
  return true;
}

bool LocalArraySubAnalyzer::analyze_aget(const IRInstruction* insn,
                                         ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_AGET_OBJECT) {
    return false;
  }
  boost::optional<int64_t> idx_opt =
      env->get_primitive(insn->src(1)).constant_domain().get_constant();
  if (!idx_opt) {
    return false;
  }
  auto arr = env->get_array(insn->src(0));
  env->set_primitive(RESULT_REGISTER, arr.get(*idx_opt));
  return true;
}

bool LocalArraySubAnalyzer::analyze_aput(const IRInstruction* insn,
                                         ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_APUT_OBJECT) {
    mark_array_unknown(insn->src(0), env);
    return false;
  }
  boost::optional<int64_t> idx_opt =
      env->get_primitive(insn->src(2)).constant_domain().get_constant();
  if (!idx_opt) {
    return false;
  }
  auto val = env->get_primitive(insn->src(0));
  env->set_array_binding(insn->src(1), *idx_opt, val);
  return true;
}

bool LocalArraySubAnalyzer::analyze_sput(const IRInstruction* insn,
                                         ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_SPUT_OBJECT) {
    mark_array_unknown(insn->src(0), env);
  }
  return false;
}

bool LocalArraySubAnalyzer::analyze_iput(const IRInstruction* insn,
                                         ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_IPUT_OBJECT) {
    mark_array_unknown(insn->src(0), env);
  }
  return false;
}

bool LocalArraySubAnalyzer::analyze_fill_array_data(const IRInstruction* insn,
                                                    ConstantEnvironment* env) {
  // We currently don't analyze fill-array-data properly; we simply
  // mark the array it modifies as unknown.
  mark_array_unknown(insn->src(0), env);
  return false;
}

bool LocalArraySubAnalyzer::analyze_invoke(const IRInstruction* insn,
                                           ConstantEnvironment* env) {
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    mark_array_unknown(insn->src(i), env);
  }
  return false;
}

// Without interprocedural escape analysis, we need to treat an object as
// being in an unknown state after it is written to a field or passed to
// another method.
void LocalArraySubAnalyzer::mark_array_unknown(reg_t reg,
                                               ConstantEnvironment* env) {
  auto ptr_opt = env->get_array_pointer(reg).get_constant();
  if (ptr_opt) {
    env->mutate_array_heap([&](ConstantArrayHeap* heap) {
      heap->set(*ptr_opt, ConstantPrimitiveArrayDomain::top());
    });
  }
}

bool ConstantPrimitiveSubAnalyzer::analyze_default(const IRInstruction* insn,
                                                   ConstantEnvironment* env) {
  if (opcode::is_load_param(insn->opcode())) {
    return true;
  }
  if (insn->dests_size()) {
    TRACE(CONSTP, 5, "Marking value unknown [Reg: %d]\n", insn->dest());
    env->set_register_to_top(insn->dest());
  } else if (insn->has_move_result() || insn->has_move_result_pseudo()) {
    TRACE(CONSTP, 5, "Clearing result register\n");
    env->set_register_to_top(RESULT_REGISTER);
  }
  return true;
}

bool ConstantPrimitiveSubAnalyzer::analyze_const(const IRInstruction* insn,
                                                 ConstantEnvironment* env) {
  TRACE(CONSTP, 5, "Discovered new constant for reg: %d value: %ld\n",
        insn->dest(), insn->get_literal());
  env->set_primitive(insn->dest(), SignedConstantDomain(insn->get_literal()));
  return true;
}

bool ConstantPrimitiveSubAnalyzer::analyze_move(const IRInstruction* insn,
                                                ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_MOVE_OBJECT) {
    // XXX gross! `const v0 0` can be either a primitive zero value or a null
    // object pointer, but we always store it as a primitive. This means that
    // we need to check both the primitive and the array environments when
    // handling move-object. Also note that we don't want to call both
    // set_primitive and set_array_pointer, because each one will unbind the
    // dest register in the other environment.
    if (!env->get_primitive(insn->src(0)).is_top()) {
      env->set_primitive(insn->dest(), env->get_primitive(insn->src(0)));
    } else {
      env->set_array_pointer(insn->dest(),
                             env->get_array_pointer(insn->src(0)));
    }
  } else {
    analyze_non_branch(insn, env);
  }
  return true;
}

bool ConstantPrimitiveSubAnalyzer::analyze_move_result(
    const IRInstruction* insn, ConstantEnvironment* env) {
  auto op = insn->opcode();
  if (op == OPCODE_MOVE_RESULT_OBJECT ||
      op == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT) {
    // See comment in MOVE_RESULT_OBJECT
    if (!env->get_primitive(RESULT_REGISTER).is_top()) {
      env->set_primitive(insn->dest(), env->get_primitive(RESULT_REGISTER));
    } else {
      env->set_array_pointer(insn->dest(),
                             env->get_array_pointer(RESULT_REGISTER));
    }
  } else {
    env->set_primitive(insn->dest(), env->get_primitive(RESULT_REGISTER));
  }
  return true;
}

bool ConstantPrimitiveSubAnalyzer::analyze_cmp(const IRInstruction* insn,
                                               ConstantEnvironment* env) {
  auto op = insn->opcode();
  switch (op) {
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
  default: {
    always_assert_log(false, "Unexpected opcode: %s\n", SHOW(op));
    break;
  }
  }
  return true;
}

bool ConstantPrimitiveSubAnalyzer::analyze_binop_lit(const IRInstruction* insn,
                                                     ConstantEnvironment* env) {
  auto op = insn->opcode();
  if (op == OPCODE_ADD_INT_LIT8 || op == OPCODE_ADD_INT_LIT16) {
    // add-int/lit8 is the most common arithmetic instruction: about .29% of
    // all instructions. All other arithmetic instructions are less than .05%
    int32_t lit = insn->get_literal();
    auto add_in_bounds = [lit](int64_t v) -> boost::optional<int64_t> {
      if (addition_out_of_bounds(lit, v)) {
        return boost::none;
      }
      return v + lit;
    };
    TRACE(CONSTP, 5, "Attempting to fold %s with literal %lu\n", SHOW(insn),
          lit);
    analyze_non_branch(insn, env, add_in_bounds);
    return true;
  }
  return analyze_default(insn, env);
}

bool ClinitFieldSubAnalyzer::analyze_sget(const DexType* class_under_init,
                                          const IRInstruction* insn,
                                          ConstantEnvironment* env) {
  auto field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return false;
  }
  if (field->get_class() == class_under_init) {
    env->set_primitive(RESULT_REGISTER, env->get_primitive(field));
    return true;
  }
  return false;
}

bool ClinitFieldSubAnalyzer::analyze_sput(const DexType* class_under_init,
                                          const IRInstruction* insn,
                                          ConstantEnvironment* env) {
  auto field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return false;
  }
  if (field->get_class() == class_under_init) {
    env->set_primitive(field, env->get_primitive(insn->src(0)));
    return true;
  }
  return false;
}

bool ClinitFieldSubAnalyzer::analyze_invoke(const DexType* class_under_init,
                                            const IRInstruction* insn,
                                            ConstantEnvironment* env) {
  // If the class initializer invokes a static method on its own class, that
  // static method can modify the class' static fields. We would have to
  // inspect the static method to find out. Here we take the conservative
  // approach of marking all static fields as unknown after the invoke.
  if (insn->opcode() == OPCODE_INVOKE_STATIC &&
      class_under_init == insn->get_method()->get_class()) {
    env->clear_field_environment();
  }
  return false;
}

namespace intraprocedural {

void FixpointIterator::analyze_instruction(const IRInstruction* insn,
                                           ConstantEnvironment* env) const {
  TRACE(CONSTP, 5, "Analyzing instruction: %s\n", SHOW(insn));
  m_insn_analyzer(insn, env);
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
                       ConstantEnvironment* env,
                       bool is_true_branch) {
  if (env->is_bottom()) {
    return;
  }
  // Inverting the conditional here means that we only need to consider the
  // "true" case of the if-* opcode
  auto op = !is_true_branch ? opcode::invert_conditional_branch(insn->opcode())
                            : insn->opcode();

  auto scd_left = env->get_primitive(insn->src(0));
  auto scd_right = insn->srcs_size() > 1 ? env->get_primitive(insn->src(1))
                                         : SignedConstantDomain(0);

  switch (op) {
  case OPCODE_IF_EQ: {
    auto refined_value = scd_left.meet(scd_right);
    env->set_primitive(insn->src(0), refined_value);
    env->set_primitive(insn->src(1), refined_value);
    break;
  }
  case OPCODE_IF_EQZ: {
    env->set_primitive(insn->src(0), scd_left.meet(SignedConstantDomain(0)));
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
      env->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_LT:
    if (scd_left.min_element() >= scd_right.max_element()) {
      env->set_to_bottom();
    }
    break;
  case OPCODE_IF_LTZ:
    env->set_primitive(
        insn->src(0),
        scd_left.meet(SignedConstantDomain(sign_domain::Interval::LTZ)));
    break;
  case OPCODE_IF_GE:
    if (scd_left.max_element() < scd_right.min_element()) {
      env->set_to_bottom();
    }
    break;
  case OPCODE_IF_GEZ:
    env->set_primitive(
        insn->src(0),
        scd_left.meet(SignedConstantDomain(sign_domain::Interval::GEZ)));
    break;
  case OPCODE_IF_GT:
    if (scd_left.max_element() <= scd_right.min_element()) {
      env->set_to_bottom();
    }
    break;
  case OPCODE_IF_GTZ:
    env->set_primitive(
        insn->src(0),
        scd_left.meet(SignedConstantDomain(sign_domain::Interval::GTZ)));
    break;
  case OPCODE_IF_LE:
    if (scd_left.min_element() > scd_right.max_element()) {
      env->set_to_bottom();
    }
    break;
  case OPCODE_IF_LEZ:
    env->set_primitive(
        insn->src(0),
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
