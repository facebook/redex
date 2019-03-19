/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationAnalysis.h"
#include <set>

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
  auto left = env->get<SignedConstantDomain>(insn->src(0)).get_constant();
  auto right = env->get<SignedConstantDomain>(insn->src(1)).get_constant();

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

static void set_escaped(reg_t reg, ConstantEnvironment* env) {
  auto ptr_opt = env->get<AbstractHeapPointer>(reg).get_constant();
  if (ptr_opt) {
    env->mutate_heap(
        [&](ConstantHeap* heap) { heap->set(*ptr_opt, HeapValue::top()); });
  }
}

bool HeapEscapeAnalyzer::analyze_aput(const IRInstruction* insn,
                                      ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_APUT_OBJECT) {
    set_escaped(insn->src(0), env);
  }
  return true;
}

bool HeapEscapeAnalyzer::analyze_sput(const IRInstruction* insn,
                                      ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_SPUT_OBJECT) {
    set_escaped(insn->src(0), env);
  }
  return true;
}

bool HeapEscapeAnalyzer::analyze_iput(const IRInstruction* insn,
                                      ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_IPUT_OBJECT) {
    set_escaped(insn->src(0), env);
  }
  return true;
}

bool HeapEscapeAnalyzer::analyze_invoke(const IRInstruction* insn,
                                        ConstantEnvironment* env) {
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    set_escaped(insn->src(i), env);
  }
  return true;
}

bool HeapEscapeAnalyzer::analyze_filled_new_array(const IRInstruction* insn,
                                                  ConstantEnvironment* env) {
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    set_escaped(insn->src(i), env);
  }
  return true;
}

bool LocalArrayAnalyzer::analyze_new_array(const IRInstruction* insn,
                                           ConstantEnvironment* env) {
  auto length = env->get<SignedConstantDomain>(insn->src(0));
  auto length_value_opt = length.get_constant();
  if (!length_value_opt) {
    return false;
  }
  env->new_heap_value(RESULT_REGISTER, insn,
                      ConstantPrimitiveArrayDomain(*length_value_opt));
  return true;
}

bool LocalArrayAnalyzer::analyze_aget(const IRInstruction* insn,
                                      ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_AGET_OBJECT) {
    return false;
  }
  boost::optional<int64_t> idx_opt =
      env->get<SignedConstantDomain>(insn->src(1)).get_constant();
  if (!idx_opt) {
    return false;
  }
  auto arr = env->get_pointee<ConstantPrimitiveArrayDomain>(insn->src(0));
  env->set(RESULT_REGISTER, arr.get(*idx_opt));
  return true;
}

bool LocalArrayAnalyzer::analyze_aput(const IRInstruction* insn,
                                      ConstantEnvironment* env) {
  if (insn->opcode() == OPCODE_APUT_OBJECT) {
    return false;
  }
  boost::optional<int64_t> idx_opt =
      env->get<SignedConstantDomain>(insn->src(2)).get_constant();
  if (!idx_opt) {
    return false;
  }
  auto val = env->get<SignedConstantDomain>(insn->src(0));
  env->set_array_binding(insn->src(1), *idx_opt, val);
  return true;
}

bool LocalArrayAnalyzer::analyze_fill_array_data(const IRInstruction* insn,
                                                 ConstantEnvironment* env) {
  // We currently don't analyze fill-array-data properly; we simply
  // mark the array it modifies as unknown.
  set_escaped(insn->src(0), env);
  return false;
}

bool PrimitiveAnalyzer::analyze_default(const IRInstruction* insn,
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

bool PrimitiveAnalyzer::analyze_const(const IRInstruction* insn,
                                      ConstantEnvironment* env) {
  TRACE(CONSTP, 5, "Discovered new constant for reg: %d value: %ld\n",
        insn->dest(), insn->get_literal());
  env->set(insn->dest(), SignedConstantDomain(insn->get_literal()));
  return true;
}

bool PrimitiveAnalyzer::analyze_move(const IRInstruction* insn,
                                     ConstantEnvironment* env) {
  env->set(insn->dest(), env->get(insn->src(0)));
  return true;
}

bool PrimitiveAnalyzer::analyze_move_result(const IRInstruction* insn,
                                            ConstantEnvironment* env) {
  env->set(insn->dest(), env->get(RESULT_REGISTER));
  return true;
}

bool PrimitiveAnalyzer::analyze_cmp(const IRInstruction* insn,
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

bool PrimitiveAnalyzer::analyze_binop_lit(const IRInstruction* insn,
                                          ConstantEnvironment* env) {
  auto op = insn->opcode();
  int32_t lit = insn->get_literal();
  TRACE(CONSTP, 5, "Attempting to fold %s with literal %lu\n", SHOW(insn), lit);
  auto cst = env->get<SignedConstantDomain>(insn->src(0)).get_constant();
  boost::optional<int64_t> result = boost::none;
  if (cst) {
    bool use_result_reg = false;
    switch (op) {
    case OPCODE_ADD_INT_LIT16:
    case OPCODE_ADD_INT_LIT8: {
      // add-int/lit8 is the most common arithmetic instruction: about .29% of
      // all instructions. All other arithmetic instructions are less than
      // .05%
      result = (*cst) + lit;
      break;
    }
    case OPCODE_RSUB_INT:
    case OPCODE_RSUB_INT_LIT8: {
      result = lit - (*cst);
      break;
    }
    case OPCODE_MUL_INT_LIT16:
    case OPCODE_MUL_INT_LIT8: {
      result = (*cst) * lit;
      break;
    }
    case OPCODE_DIV_INT_LIT16:
    case OPCODE_DIV_INT_LIT8: {
      if (lit != 0) {
        result = (*cst) / lit;
      }
      use_result_reg = true;
      break;
    }
    case OPCODE_REM_INT_LIT16:
    case OPCODE_REM_INT_LIT8: {
      if (lit != 0) {
        result = (*cst) % lit;
      }
      use_result_reg = true;
      break;
    }
    case OPCODE_AND_INT_LIT16:
    case OPCODE_AND_INT_LIT8: {
      result = (*cst) & lit;
      break;
    }
    case OPCODE_OR_INT_LIT16:
    case OPCODE_OR_INT_LIT8: {
      result = (*cst) | lit;
      break;
    }
    case OPCODE_XOR_INT_LIT16:
    case OPCODE_XOR_INT_LIT8: {
      result = (*cst) ^ lit;
      break;
    }
    // as in https://source.android.com/devices/tech/dalvik/dalvik-bytecode
    // the following operations have the second operand masked.
    case OPCODE_SHL_INT_LIT8: {
      uint32_t ucst = *cst;
      uint32_t uresult = ucst << (lit & 0x1f);
      result = (int32_t)uresult;
      break;
    }
    case OPCODE_SHR_INT_LIT8: {
      result = (*cst) >> (lit & 0x1f);
      break;
    }
    case OPCODE_USHR_INT_LIT8: {
      uint32_t ucst = *cst;
      // defined in dalvik spec
      result = ucst >> (lit & 0x1f);
      break;
    }
    default:
      break;
    }
    auto res_const_dom = SignedConstantDomain::top();
    if (result != boost::none) {
      int32_t result32 = (int32_t)(*result & 0xFFFFFFFF);
      res_const_dom = SignedConstantDomain(result32);
    }
    env->set(use_result_reg ? RESULT_REGISTER : insn->dest(), res_const_dom);
    return true;
  }
  return analyze_default(insn, env);
}

bool ClinitFieldAnalyzer::analyze_sget(const DexType* class_under_init,
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

bool ClinitFieldAnalyzer::analyze_sput(const DexType* class_under_init,
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

bool ClinitFieldAnalyzer::analyze_invoke(const DexType* class_under_init,
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

bool InitFieldAnalyzer::analyze_iget(const DexType* class_under_init,
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

bool InitFieldAnalyzer::analyze_iput(const DexType* class_under_init,
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

bool EnumFieldAnalyzer::analyze_sget(const EnumFieldAnalyzerState&,
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
  // Note that EnumFieldAnalyzer assumes that it is the only one in a
  // combined chain of Analyzers that creates SingletonObjectDomains of Enum
  // types.
  env->set(RESULT_REGISTER, SingletonObjectDomain(field));
  return true;
}

bool EnumFieldAnalyzer::analyze_invoke(const EnumFieldAnalyzerState& state,
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

bool BoxedBooleanAnalyzer::analyze_sget(const BoxedBooleanAnalyzerState& state,
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
  // Note that BoxedBooleanAnalyzer assumes that it is the only one in a
  // combined chain of Analyzers that creates SingletonObjectDomains of
  // Boolean type.
  if (field != state.boolean_true && field != state.boolean_false) {
    return false;
  }
  env->set(RESULT_REGISTER, SingletonObjectDomain(field));
  return true;
}

bool BoxedBooleanAnalyzer::analyze_invoke(
    const BoxedBooleanAnalyzerState& state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  auto method = insn->get_method();
  if (method == nullptr || method->get_class() != state.boolean_class) {
    return false;
  }
  if (method == state.boolean_valueof) {
    auto cst = env->get<SignedConstantDomain>(insn->src(0)).get_constant();
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

void semantically_inline_method(
    IRCode* callee_code,
    const IRInstruction* insn,
    const InstructionAnalyzer<ConstantEnvironment>& analyzer,
    ConstantEnvironment* env) {
  callee_code->build_cfg(/* editable */ false);
  auto& cfg = callee_code->cfg();

  // Set up the environment at entry into the callee.
  ConstantEnvironment call_entry_env;
  auto load_params = callee_code->get_param_instructions();
  auto load_params_it = InstructionIterable(load_params).begin();
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    call_entry_env.set(load_params_it->insn->dest(), env->get(insn->src(i)));
    ++load_params_it;
  }
  call_entry_env.mutate_heap(
      [env](ConstantHeap* heap) { *heap = env->get_heap(); });

  // Analyze the callee.
  auto fp_iter =
      std::make_unique<intraprocedural::FixpointIterator>(cfg, analyzer);
  fp_iter->run(call_entry_env);

  // Update the caller's environment with the callee's return states.
  auto return_state = collect_return_state(callee_code, *fp_iter);
  env->set(RESULT_REGISTER, return_state.get_value());
  env->mutate_heap(
      [&return_state](ConstantHeap* heap) { *heap = return_state.get_heap(); });
}

ReturnState collect_return_state(
    IRCode* code, const intraprocedural::FixpointIterator& fp_iter) {
  auto& cfg = code->cfg();
  auto return_state = ReturnState::bottom();
  for (cfg::Block* b : cfg.blocks()) {
    auto env = fp_iter.get_entry_state_at(b);
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      fp_iter.analyze_instruction(insn, &env);
      if (is_return(insn->opcode())) {
        if (insn->opcode() != OPCODE_RETURN_VOID) {
          return_state.join_with(
              ReturnState(env.get(insn->dest()), env.get_heap()));
        } else {
          return_state.join_with(
              ReturnState(ConstantValue::top(), env.get_heap()));
        }
      }
    }
  }
  return return_state;
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
    if (ConstantValue::apply_visitor(runtime_lt_visitor(), left, right)) {
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
    if (ConstantValue::apply_visitor(runtime_lt_visitor(), right, left)) {
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
    const EdgeId& edge,
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
  } else if (is_switch(op)) {
    const auto& case_key = edge->case_key();
    if (case_key) {
      env.set(insn->src(0),
              env.get(insn->src(0)).meet(SignedConstantDomain(*case_key)));
    }
  }
  return env;
}

} // namespace intraprocedural

} // namespace constant_propagation
