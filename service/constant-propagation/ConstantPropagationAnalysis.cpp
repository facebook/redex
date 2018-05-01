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
    env->set(insn->dest(), SignedConstantDomain(result));
  } else {
    env->set(insn->dest(), SignedConstantDomain::top());
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
  env->set(RESULT_REGISTER, arr.get(*idx_opt));
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
  auto ptr_opt = env->get_pointer(reg).get_constant();
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
    env->set(insn->dest(), ConstantValue::top());
  } else if (insn->has_move_result() || insn->has_move_result_pseudo()) {
    TRACE(CONSTP, 5, "Clearing result register\n");
    env->set(RESULT_REGISTER, ConstantValue::top());
  }
  return true;
}

bool ConstantPrimitiveSubAnalyzer::analyze_const(const IRInstruction* insn,
                                                 ConstantEnvironment* env) {
  TRACE(CONSTP, 5, "Discovered new constant for reg: %d value: %ld\n",
        insn->dest(), insn->get_literal());
  env->set(insn->dest(), SignedConstantDomain(insn->get_literal()));
  return true;
}

bool ConstantPrimitiveSubAnalyzer::analyze_move(const IRInstruction* insn,
                                                ConstantEnvironment* env) {
  env->set(insn->dest(), env->get(insn->src(0)));
  return true;
}

bool ConstantPrimitiveSubAnalyzer::analyze_move_result(
    const IRInstruction* insn, ConstantEnvironment* env) {
  env->set(insn->dest(), env->get(RESULT_REGISTER));
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
    TRACE(CONSTP, 5, "Attempting to fold %s with literal %lu\n", SHOW(insn),
          lit);

    auto result = SignedConstantDomain::top();
    auto cst =
        env->get_primitive(insn->src(0)).constant_domain().get_constant();
    if (cst && !addition_out_of_bounds(lit, *cst)) {
      result = SignedConstantDomain(*cst + lit);
    }
    env->set(insn->dest(), result);
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
    env->set(RESULT_REGISTER, env->get(field));
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
    env->set(field, env->get(insn->src(0)));
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

bool EnumFieldSubAnalyzer::analyze_sget(const EnumFieldSubAnalyzerState&,
                                        const IRInstruction* insn,
                                        ConstantEnvironment* env) {
  if (insn->opcode() != OPCODE_SGET_OBJECT) {
    return false;
  }
  auto field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return false;
  }
  if (!is_enum(field)) {
    return false;
  }
  // An enum value is compiled into a static final field of the enum class.
  // Each of these fields contain a unique object, so we can represent them
  // with a SingletonObjectDomain.
  // Note that EnumFieldSubAnalyzer assumes that it is the only one in a
  // combined chain of SubAnalyzers that creates SingletonObjectDomains of Enum
  // types.
  env->set(RESULT_REGISTER, SingletonObjectDomain(field));
  return true;
}

bool EnumFieldSubAnalyzer::analyze_invoke(
    const EnumFieldSubAnalyzerState& state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  auto op = insn->opcode();
  if (op == OPCODE_INVOKE_VIRTUAL) {
    auto* method = resolve_method(insn->get_method(), MethodSearch::Virtual);
    if (method == nullptr) {
      return false;
    }
    if (method == state.enum_equals) {
      auto left = env->get(insn->src(0)).maybe_get<SingletonObjectDomain>();
      auto right = env->get(insn->src(1)).maybe_get<SingletonObjectDomain>();
      if (!left || !right) {
        return false;
      }
      auto left_field = left->get_constant();
      auto right_field = right->get_constant();
      if (!left_field || !right_field) {
        return false;
      }
      env->set(RESULT_REGISTER,
               SignedConstantDomain(left_field == right_field));
      return true;
    }
  }
  return false;
}

bool BoxedBooleanSubAnalyzer::analyze_sget(
    const BoxedBooleanSubAnalyzerState& state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  if (insn->opcode() != OPCODE_SGET_OBJECT) {
    return false;
  }
  auto field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return false;
  }
  // Boolean.TRUE and Boolean.FALSE each contain a unique object, so we can
  // represent them with a SingletonObjectDomain.
  // Note that BoxedBooleanSubAnalyzer assumes that it is the only one in a
  // combined chain of SubAnalyzers that creates SingletonObjectDomains of
  // Boolean type.
  if (field != state.boolean_true && field != state.boolean_false) {
    return false;
  }
  env->set(RESULT_REGISTER, SingletonObjectDomain(field));
  return true;
}

bool BoxedBooleanSubAnalyzer::analyze_invoke(
    const BoxedBooleanSubAnalyzerState& state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  auto method = insn->get_method();
  if (method == nullptr || method->get_class() != state.boolean_class) {
    return false;
  }
  if (method == state.boolean_valueof) {
    auto cst =
        env->get_primitive(insn->src(0)).constant_domain().get_constant();
    if (!cst) {
      return false;
    }
    if (*cst == 0) {
      env->set(RESULT_REGISTER, SingletonObjectDomain(state.boolean_false));
    } else {
      env->set(RESULT_REGISTER, SingletonObjectDomain(state.boolean_true));
    }
    return true;
  } else if (method == state.boolean_booleanvalue) {
    auto value = env->get(insn->src(0)).maybe_get<SingletonObjectDomain>();
    if (!value) {
      return false;
    }
    auto cst = value->get_constant();
    if (!cst) {
      return false;
    }
    if (cst == state.boolean_false) {
      env->set(RESULT_REGISTER, SignedConstantDomain(0));
      return true;
    } else if (cst == state.boolean_true) {
      env->set(RESULT_REGISTER, SignedConstantDomain(1));
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
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
 * Note that runtime_equals_visitor and runtime_leq_visitor are handling
 * different notions of equality / order than AbstractDomain::equals() and
 * AbstractDomain::leq(). The former return true if they can prove that their
 * respective relations hold for a runtime comparison (e.g. from an if-eq or
 * packed-switch instruction). In contrast, AbstractDomain::equals() will
 * return true for two domains representing integers > 0, even though their
 * corresponding runtime values may be different integers.
 */
class runtime_equals_visitor : public boost::static_visitor<bool> {
 public:
  bool operator()(const SignedConstantDomain& scd_left,
                  const SignedConstantDomain& scd_right) const {
    auto cd_left = scd_left.constant_domain();
    auto cd_right = scd_right.constant_domain();
    if (!(cd_left.is_value() && cd_right.is_value())) {
      return false;
    }
    if (*cd_left.get_constant() == *cd_right.get_constant()) {
      return true;
    }
    return false;
  }

  bool operator()(const SingletonObjectDomain& d1,
                  const SingletonObjectDomain& d2) const {
    if (!(d1.is_value() && d2.is_value())) {
      return false;
    }
    if (*d1.get_constant() == *d2.get_constant()) {
      return true;
    }
    return false;
  }

  template <typename Domain, typename OtherDomain>
  bool operator()(const Domain& d1, const OtherDomain& d2) const {
    return false;
  }
};

class runtime_leq_visitor : public boost::static_visitor<bool> {
 public:
  bool operator()(const SignedConstantDomain& scd_left,
                  const SignedConstantDomain& scd_right) const {
    return scd_left.max_element() <= scd_right.min_element();
  }

  template <typename Domain, typename OtherDomain>
  bool operator()(const Domain& d1, const OtherDomain& d2) const {
    return false;
  }
};

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

  auto left = env->get(insn->src(0));
  auto right =
      insn->srcs_size() > 1 ? env->get(insn->src(1)) : SignedConstantDomain(0);

  switch (op) {
  case OPCODE_IF_EQ: {
    auto refined_value = left.meet(right);
    env->set(insn->src(0), refined_value);
    env->set(insn->src(1), refined_value);
    break;
  }
  case OPCODE_IF_EQZ: {
    env->set(insn->src(0), left.meet(SignedConstantDomain(0)));
    break;
  }
  case OPCODE_IF_NE:
  case OPCODE_IF_NEZ: {
    if (ConstantValue::apply_visitor(runtime_equals_visitor(), left, right)) {
      env->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_LT: {
    if (ConstantValue::apply_visitor(runtime_leq_visitor(), right, left)) {
      env->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_LTZ: {
    env->set(insn->src(0),
             left.meet(SignedConstantDomain(sign_domain::Interval::LTZ)));
    break;
  }
  case OPCODE_IF_GT: {
    if (ConstantValue::apply_visitor(runtime_leq_visitor(), left, right)) {
      env->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_GTZ: {
    env->set(insn->src(0),
             left.meet(SignedConstantDomain(sign_domain::Interval::GTZ)));
    break;
  }
  case OPCODE_IF_GE: {
    if (ConstantValue::apply_visitor(runtime_leq_visitor(), left, right) &&
        !ConstantValue::apply_visitor(runtime_equals_visitor(), left, right)) {
      env->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_GEZ: {
    env->set(insn->src(0),
             left.meet(SignedConstantDomain(sign_domain::Interval::GEZ)));
    break;
  }
  case OPCODE_IF_LE: {
    if (ConstantValue::apply_visitor(runtime_leq_visitor(), right, left) &&
        !ConstantValue::apply_visitor(runtime_equals_visitor(), left, right)) {
      env->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_LEZ: {
    env->set(insn->src(0),
             left.meet(SignedConstantDomain(sign_domain::Interval::LEZ)));
    break;
  }
  default: {
    always_assert_log(false, "expected if-* opcode, got %s", SHOW(insn));
    not_reached();
  }
  }
}

ConstantEnvironment FixpointIterator::analyze_edge(
    const std::shared_ptr<cfg::Edge>& edge,
    const ConstantEnvironment& exit_state_at_source) const {
  auto env = exit_state_at_source;
  auto last_insn_it = edge->src()->get_last_insn();
  if (last_insn_it == edge->src()->end()) {
    return env;
  }

  auto insn = last_insn_it->insn;
  auto op = insn->opcode();
  if (is_conditional_branch(op)) {
    analyze_if(insn, &env, edge->type() == cfg::EDGE_BRANCH);
  }
  return env;
}

} // namespace intraprocedural

} // namespace constant_propagation
