/*
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

// While undefined behavior C++-wise, the two's complement implementation of
// modern processors matches the required Java semantics. So silence ubsan.
#if defined(__clang__)
#define NO_UBSAN_ARITH \
  __attribute__((no_sanitize("signed-integer-overflow", "shift")))
#else
#define NO_UBSAN_ARITH
#endif

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
          "Operands [%d] [%d] -> Result: [%d]",
          SHOW(insn), l_val, r_val, result);
    env->set(insn->dest(), SignedConstantDomain(result));
  } else {
    env->set(insn->dest(), SignedConstantDomain::top());
  }
}

} // namespace

namespace constant_propagation {

static void set_escaped(reg_t reg, ConstantEnvironment* env) {
  if (auto ptr_opt = env->get(reg).maybe_get<AbstractHeapPointer>()) {
    if (auto ptr_value = ptr_opt->get_constant()) {
      env->mutate_heap(
          [&](ConstantHeap* heap) { heap->set(*ptr_value, HeapValue::top()); });
    }
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
  switch (insn->opcode()) {
  case OPCODE_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY:
  case OPCODE_NEW_INSTANCE:
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_CLASS: {
    env->set(RESULT_REGISTER, SignedConstantDomain(sign_domain::Interval::NEZ));
    return true;
  }
  case OPCODE_MOVE_EXCEPTION: {
    env->set(insn->dest(), SignedConstantDomain(sign_domain::Interval::NEZ));
    return true;
  }
  case OPCODE_ARRAY_LENGTH: {
    env->set(RESULT_REGISTER, SignedConstantDomain(sign_domain::Interval::GEZ));
    return true;
  }
  default:
    break;
  }
  if (insn->has_dest()) {
    TRACE(CONSTP, 5, "Marking value unknown [Reg: %d] %s", insn->dest(),
          SHOW(insn));
    env->set(insn->dest(), ConstantValue::top());
  } else if (insn->has_move_result_any()) {
    TRACE(CONSTP, 5, "Clearing result register %s", SHOW(insn));
    env->set(RESULT_REGISTER, ConstantValue::top());
  }
  return true;
}

bool PrimitiveAnalyzer::analyze_const(const IRInstruction* insn,
                                      ConstantEnvironment* env) {
  TRACE(CONSTP, 5, "Discovered new constant for reg: %d value: %ld",
        insn->dest(), insn->get_literal());
  env->set(insn->dest(), SignedConstantDomain(insn->get_literal()));
  return true;
}

bool PrimitiveAnalyzer::analyze_instance_of(const IRInstruction* insn,
                                            ConstantEnvironment* env) {
  auto src = env->get(insn->src(0)).maybe_get<SignedConstantDomain>();
  if (src && src->get_constant() && *(src->get_constant()) == 0) {
    env->set(RESULT_REGISTER, SignedConstantDomain(0));
    return true;
  }
  return analyze_default(insn, env);
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

bool PrimitiveAnalyzer::analyze_binop_lit(
    const IRInstruction* insn, ConstantEnvironment* env) NO_UBSAN_ARITH {
  auto op = insn->opcode();
  int32_t lit = insn->get_literal();
  TRACE(CONSTP, 5, "Attempting to fold %s with literal %lu", SHOW(insn), lit);
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

bool is_binop64(IROpcode op) {
  switch (op) {
  case OPCODE_ADD_INT:
  case OPCODE_SUB_INT:
  case OPCODE_MUL_INT:
  case OPCODE_DIV_INT:
  case OPCODE_REM_INT:
  case OPCODE_AND_INT:
  case OPCODE_OR_INT:
  case OPCODE_XOR_INT:
  case OPCODE_SHL_INT:
  case OPCODE_SHR_INT:
  case OPCODE_USHR_INT:
  case OPCODE_ADD_FLOAT:
  case OPCODE_SUB_FLOAT:
  case OPCODE_MUL_FLOAT:
  case OPCODE_DIV_FLOAT:
  case OPCODE_REM_FLOAT: {
    return false;
    break;
  }
  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG:
  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG:
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE: {
    return true;
    break;
  }
  default: {
    always_assert_log(false, "Unexpected opcode: %s\n", SHOW(op));
    break;
  }
  }
}

bool PrimitiveAnalyzer::analyze_binop(const IRInstruction* insn,
                                      ConstantEnvironment* env) NO_UBSAN_ARITH {
  auto op = insn->opcode();
  TRACE(CONSTP, 5, "Attempting to fold %s", SHOW(insn));
  auto cst_left = env->get<SignedConstantDomain>(insn->src(0)).get_constant();
  auto cst_right = env->get<SignedConstantDomain>(insn->src(1)).get_constant();
  boost::optional<int64_t> result = boost::none;
  if (cst_left && cst_right) {
    bool use_result_reg = false;
    switch (op) {
    case OPCODE_ADD_INT:
    case OPCODE_ADD_LONG: {
      result = (*cst_left) + (*cst_right);
      break;
    }
    case OPCODE_SUB_INT:
    case OPCODE_SUB_LONG: {
      result = (*cst_left) - (*cst_right);
      break;
    }
    case OPCODE_MUL_INT:
    case OPCODE_MUL_LONG: {
      result = (*cst_left) * (*cst_right);
      break;
    }
    case OPCODE_DIV_INT:
    case OPCODE_DIV_LONG: {
      if ((*cst_right) != 0) {
        result = (*cst_left) / (*cst_right);
      }
      use_result_reg = true;
      break;
    }
    case OPCODE_REM_INT:
    case OPCODE_REM_LONG: {
      if ((*cst_right) != 0) {
        result = (*cst_left) % (*cst_right);
      }
      use_result_reg = true;
      break;
    }
    case OPCODE_AND_INT:
    case OPCODE_AND_LONG: {
      result = (*cst_left) & (*cst_right);
      break;
    }
    case OPCODE_OR_INT:
    case OPCODE_OR_LONG: {
      result = (*cst_left) | (*cst_right);
      break;
    }
    case OPCODE_XOR_INT:
    case OPCODE_XOR_LONG: {
      result = (*cst_left) ^ (*cst_right);
      break;
    }
    default:
      return analyze_default(insn, env);
    }
    auto res_const_dom = SignedConstantDomain::top();
    if (result != boost::none) {
      if (is_binop64(op)) {
        res_const_dom = SignedConstantDomain(*result);
      } else {
        int32_t result32 = (int32_t)(*result & 0xFFFFFFFF);
        res_const_dom = SignedConstantDomain(result32);
      }
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

boost::optional<EnumFieldAnalyzerState> enum_field_singleton{boost::none};
const EnumFieldAnalyzerState& EnumFieldAnalyzerState::get() {
  if (!enum_field_singleton) {
    // Be careful, there could be a data race here if this is called in parallel
    enum_field_singleton = EnumFieldAnalyzerState();
    // In tests, we create and destroy g_redex repeatedly. So we need to reset
    // the singleton.
    g_redex->add_destruction_task([]() { enum_field_singleton = boost::none; });
  }
  return *enum_field_singleton;
}

boost::optional<BoxedBooleanAnalyzerState> boxed_boolean_singleton{boost::none};
const BoxedBooleanAnalyzerState& BoxedBooleanAnalyzerState::get() {
  if (!boxed_boolean_singleton) {
    // Be careful, there could be a data race here if this is called in parallel
    boxed_boolean_singleton = BoxedBooleanAnalyzerState();
    // In tests, we create and destroy g_redex repeatedly. So we need to reset
    // the singleton.
    g_redex->add_destruction_task(
        []() { boxed_boolean_singleton = boost::none; });
  }
  return *boxed_boolean_singleton;
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

ImmutableAttributeAnalyzerState::Initializer&
ImmutableAttributeAnalyzerState::add_initializer(DexMethod* initialize_method,
                                                 DexMethod* attr) {
  attribute_methods.insert(attr);
  auto& initializers = method_initializers[initialize_method];
  initializers.push_back(Initializer(attr));
  return initializers.back();
}

ImmutableAttributeAnalyzerState::Initializer&
ImmutableAttributeAnalyzerState::add_initializer(DexMethod* initialize_method,
                                                 DexField* attr) {
  attribute_fields.insert(attr);
  auto& initializers = method_initializers[initialize_method];
  initializers.push_back(Initializer(attr));
  return initializers.back();
}

ImmutableAttributeAnalyzerState::ImmutableAttributeAnalyzerState() {
  // clang-format off
  // Integer can be initialized throuth
  //  invoke-static v0 Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;
  // clang-format on
  auto integer_valueOf = method::java_lang_Integer_valueOf();
  auto integer_intValue = method::java_lang_Integer_intValue();
  // The intValue of integer is initialized through the static invocation.
  add_initializer(integer_valueOf, integer_intValue)
      .set_src_id_of_attr(0)
      .set_obj_to_dest();
}

bool ImmutableAttributeAnalyzer::analyze_iget(
    const ImmutableAttributeAnalyzerState& state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  auto field_ref = insn->get_field();
  DexField* field = resolve_field(field_ref, FieldSearch::Instance);
  if (!field) {
    field = static_cast<DexField*>(field_ref);
  }
  if (!state.attribute_fields.count(field)) {
    return false;
  }
  auto heap_obj = env->get_pointee<ObjectWithImmutAttrDomain>(insn->src(0));
  if (heap_obj.is_top()) {
    return false;
  }
  auto value = heap_obj.get_constant()->get_value(field);
  if (value && !value->is_top()) {
    if (const auto& string_value = value->maybe_get<StringDomain>()) {
      env->set(RESULT_REGISTER, *string_value);
      return true;
    } else if (const auto& signed_value =
                   value->maybe_get<SignedConstantDomain>()) {
      env->set(RESULT_REGISTER, *signed_value);
      return true;
    }
  }
  return false;
}

bool ImmutableAttributeAnalyzer::analyze_invoke(
    const ImmutableAttributeAnalyzerState& state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  auto method_ref = insn->get_method();
  DexMethod* method = resolve_method(method_ref, opcode_to_search(insn));
  if (!method) {
    // Redex may run without sdk as input, so the method resolving may fail.
    // Example: Integer.valueOf(I) is an external method.
    method = static_cast<DexMethod*>(method_ref);
  }
  if (state.method_initializers.count(method)) {
    return analyze_method_initialization(state, insn, env, method);
  } else if (state.attribute_methods.count(method)) {
    return analyze_method_attr(state, insn, env, method);
  }
  return false;
}

/**
 * Propagate method return value if this method is a getter method of an
 * immutable field of an object. `Integer.intValue()` is a such method.
 */
bool ImmutableAttributeAnalyzer::analyze_method_attr(
    const ImmutableAttributeAnalyzerState& /* unused */,
    const IRInstruction* insn,
    ConstantEnvironment* env,
    DexMethod* method) {
  if (insn->srcs_size() != 1) {
    return false;
  }
  auto heap_obj = env->get_pointee<ObjectWithImmutAttrDomain>(insn->src(0));
  if (heap_obj.is_top()) {
    return false;
  }
  auto value = (heap_obj.get_constant())->get_value(method);
  if (value && !value->is_top()) {
    if (const auto& string_value = value->maybe_get<StringDomain>()) {
      env->set(RESULT_REGISTER, *string_value);
      return true;
    } else if (const auto& signed_value =
                   value->maybe_get<SignedConstantDomain>()) {
      env->set(RESULT_REGISTER, *signed_value);
      return true;
    }
  }
  return false;
}

bool ImmutableAttributeAnalyzer::analyze_method_initialization(
    const ImmutableAttributeAnalyzerState& state,
    const IRInstruction* insn,
    ConstantEnvironment* env,
    DexMethod* method) {
  auto it = state.method_initializers.find(method);
  if (it == state.method_initializers.end()) {
    return false;
  }
  ObjectWithImmutAttr heap_obj;
  // Only support one register for the object, can be easily extended. For
  // example, virtual method may return `this` pointer, so two registers are
  // holding the same heap object.
  reg_t obj_reg;
  for (auto& initializer : it->second) {
    obj_reg = initializer.obj_is_dest()
                  ? RESULT_REGISTER
                  : insn->src(*initializer.insn_src_id_of_obj);
    const auto& domain = env->get(insn->src(initializer.insn_src_id_of_attr));
    if (const auto& signed_value = domain.maybe_get<SignedConstantDomain>()) {
      if (!signed_value->get_constant()) {
        continue;
      }
      heap_obj.write_value(initializer.attr, *signed_value);
    } else if (const auto& string_value = domain.maybe_get<StringDomain>()) {
      if (!string_value->is_value()) {
        continue;
      }
      heap_obj.write_value(initializer.attr, *string_value);
    }
  }
  if (heap_obj.empty()) {
    return false;
  }
  env->new_heap_value(obj_reg, insn, ObjectWithImmutAttrDomain(heap_obj));
  return true;
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
  TRACE(CONSTP, 5, "Analyzing instruction: %s", SHOW(insn));
  m_insn_analyzer(insn, env);
}

void FixpointIterator::analyze_node(const NodeId& block,
                                    ConstantEnvironment* state_at_entry) const {
  TRACE(CONSTP, 5, "Analyzing block: %d", block->id());
  for (auto& mie : InstructionIterable(block)) {
    analyze_instruction(mie.insn, state_at_entry);
  }
}

/*
 * Helpers for CFG edge analysis
 */

/*
 * If we can determine that a branch is not taken based on the constants in
 * the environment, set the environment to bottom upon entry into the
 * unreachable block.
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
    auto value = env->get(insn->src(0)).maybe_get<SignedConstantDomain>();
    if (value && value->interval() == sign_domain::Interval::NEZ) {
      env->set_to_bottom();
      break;
    }
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
    const EdgeId& edge, const ConstantEnvironment& exit_state_at_source) const {
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
    auto selector_val = env.get(insn->src(0));
    const auto& case_key = edge->case_key();
    if (case_key) {
      env.set(insn->src(0), selector_val.meet(SignedConstantDomain(*case_key)));
    } else if (edge->type() == cfg::EDGE_GOTO) {
      // We are looking at the fallthrough case. Set env to bottom in case there
      // is a non-fallthrough edge with a case-key that is equal to the actual
      // selector value.
      for (auto succ : edge->src()->succs()) {
        const auto& succ_case_key = succ->case_key();
        if (succ_case_key && ConstantValue::apply_visitor(
                                 runtime_equals_visitor(), selector_val,
                                 SignedConstantDomain(*succ_case_key))) {
          env.set_to_bottom();
          break;
        }
      }
    }
  }
  return env;
}

} // namespace intraprocedural

} // namespace constant_propagation
