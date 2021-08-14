/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationAnalysis.h"

#include <boost/functional/hash.hpp>
#include <cinttypes>
#include <mutex>
#include <set>

#include "RedexContext.h"

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
#include "Trace.h"
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
          SHOW(insn), (int)l_val, (int)r_val, result);
    env->set(insn->dest(), SignedConstantDomain(result));
  } else {
    env->set(insn->dest(), SignedConstantDomain::top());
  }
}

bool is_zero(boost::optional<SignedConstantDomain> src) {
  return src && src->get_constant() && *(src->get_constant()) == 0;
}
} // namespace

namespace constant_propagation {

boost::optional<size_t> get_null_check_object_index(
    const IRInstruction* insn,
    const std::unordered_set<DexMethodRef*>& kotlin_null_check_assertions) {
  switch (insn->opcode()) {
  case OPCODE_INVOKE_STATIC: {
    auto method = insn->get_method();
    if (kotlin_null_check_assertions.count(method)) {
      // Note: We are not assuming here that the first argument is the checked
      // argument of type object, as it might not be. For example,
      // RemoveUnusedArgs may have removed or otherwise reordered the arguments.
      // TODO: Don't mattern match at all, but make this a deep semantic
      // analysis, as even this remaining pattern matching is brittle once we
      // might start doing argument type weakening / strengthening
      // optimizations.
      auto& args = *method->get_proto()->get_args();
      for (size_t i = 0; i < args.size(); i++) {
        if (args.at(i) == type::java_lang_Object()) {
          return i;
        }
      }
    }
    break;
  }
  default:
    break;
  }
  return boost::none;
}

boost::optional<size_t> get_dereferenced_object_src_index(
    const IRInstruction* insn) {
  switch (insn->opcode()) {
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_AGET:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_WIDE:
  case OPCODE_AGET_SHORT:
  case OPCODE_AGET_OBJECT:
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_IGET:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_WIDE:
  case OPCODE_IGET_SHORT:
  case OPCODE_IGET_OBJECT:
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_ARRAY_LENGTH:
  case OPCODE_FILL_ARRAY_DATA:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_INTERFACE:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_DIRECT:
    return 0;
  case OPCODE_APUT:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_SHORT:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_IPUT:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_SHORT:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
    return 1;
  default:
    return boost::none;
  }
}

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
  if (opcode::is_a_load_param(insn->opcode())) {
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
  TRACE(CONSTP, 5, "Discovered new constant for reg: %d value: %" PRIu64,
        insn->dest(), insn->get_literal());
  env->set(insn->dest(), SignedConstantDomain(insn->get_literal()));
  return true;
}

bool PrimitiveAnalyzer::analyze_check_cast(const IRInstruction* insn,
                                           ConstantEnvironment* env) {
  auto src = env->get(insn->src(0)).maybe_get<SignedConstantDomain>();
  if (is_zero(src)) {
    env->set(RESULT_REGISTER, SignedConstantDomain(0));
    return true;
  }
  return analyze_default(insn, env);
}

bool PrimitiveAnalyzer::analyze_instance_of(const IRInstruction* insn,
                                            ConstantEnvironment* env) {
  auto src = env->get(insn->src(0)).maybe_get<SignedConstantDomain>();
  if (is_zero(src)) {
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
    not_reached_log("Unexpected opcode: %s\n", SHOW(op));
  }
  }
  return true;
}

bool PrimitiveAnalyzer::analyze_binop_lit(
    const IRInstruction* insn, ConstantEnvironment* env) NO_UBSAN_ARITH {
  auto op = insn->opcode();
  int32_t lit = insn->get_literal();
  TRACE(CONSTP, 5, "Attempting to fold %s with literal %d", SHOW(insn), lit);
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
      if (opcode::is_binop64(op)) {
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

bool InitFieldAnalyzer::analyze_invoke(const DexType* class_under_init,
                                       const IRInstruction* insn,
                                       ConstantEnvironment* env) {
  // If the class initializer invokes a method on its own class, that
  // method can modify the class' fields. We would have to inspect the
  // method to find out. Here we take the conservative approach of
  // marking all fields as unknown after the invoke.
  auto opcode = insn->opcode();
  if ((opcode == OPCODE_INVOKE_VIRTUAL || opcode == OPCODE_INVOKE_DIRECT) &&
      class_under_init == insn->get_method()->get_class()) {
    env->clear_field_environment();
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
  ImmutableAttributeAnalyzerState::Initializer* new_initializer = nullptr;
  method_initializers.update(
      initialize_method,
      [&](DexMethod*, std::vector<Initializer>& initializers, bool) {
        initializers.push_back(Initializer(attr));
        new_initializer = &initializers.back();
      });
  redex_assert(new_initializer);
  return *new_initializer;
}

ImmutableAttributeAnalyzerState::Initializer&
ImmutableAttributeAnalyzerState::add_initializer(DexMethod* initialize_method,
                                                 DexField* attr) {
  attribute_fields.insert(attr);
  ImmutableAttributeAnalyzerState::Initializer* new_initializer = nullptr;
  method_initializers.update(
      initialize_method,
      [&](DexMethod*, std::vector<Initializer>& initializers, bool) {
        initializers.push_back(Initializer(attr));
        new_initializer = &initializers.back();
      });
  redex_assert(new_initializer);
  return *new_initializer;
}

ImmutableAttributeAnalyzerState::Initializer&
ImmutableAttributeAnalyzerState::add_initializer(
    DexMethod* initialize_method, const ImmutableAttr::Attr& attr) {
  return attr.is_field() ? add_initializer(initialize_method, attr.field)
                         : add_initializer(initialize_method, attr.method);
}

ImmutableAttributeAnalyzerState::ImmutableAttributeAnalyzerState() {
  // clang-format off
  // Integer can be initialized throuth
  //  invoke-static v0 Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;
  // clang-format on
  // Other boxed types are similar.
  struct BoxedTypeInfo {
    DexType* type;
    long begin;
    long end;
  };
  // See e.g.
  // https://cs.android.com/android/platform/superproject/+/master:libcore/ojluni/src/main/java/java/lang/Integer.java
  // for what is actually cached on Android. Note:
  // - We don't handle java.lang.Boolean here, as that's more appropriate
  // handled by the
  //   BoxedBooleanAnalyzer, which also knows about the FALSE and TRUE fields.
  // - The actual upper bound of cached Integers is actually configurable. We
  //   just use the minimum value here.
  std::array<BoxedTypeInfo, 7> boxed_type_infos = {
      BoxedTypeInfo{type::java_lang_Byte(), -128, 128},
      BoxedTypeInfo{type::java_lang_Short(), -128, 128},
      BoxedTypeInfo{type::java_lang_Character(), 0, 128},
      BoxedTypeInfo{type::java_lang_Integer(), -128, 128},
      BoxedTypeInfo{type::java_lang_Long(), -128, 128},
      BoxedTypeInfo{type::java_lang_Float(), 0, 0},
      BoxedTypeInfo{type::java_lang_Double(), 0, 0}};
  for (auto& bti : boxed_type_infos) {
    auto valueOf = type::get_value_of_method_for_type(bti.type);
    auto getter_method = type::get_unboxing_method_for_type(bti.type);
    if (valueOf && getter_method && valueOf->is_def() &&
        getter_method->is_def()) {
      add_initializer(valueOf->as_def(), getter_method->as_def())
          .set_src_id_of_attr(0)
          .set_obj_to_dest();
      if (bti.end > bti.begin) {
        add_cached_boxed_objects(valueOf->as_def(), bti.begin, bti.end);
      }
    }
  }
}

void ImmutableAttributeAnalyzerState::add_cached_boxed_objects(
    DexMethod* initialize_method, long begin, long end) {
  always_assert(begin < end);
  cached_boxed_objects.emplace(initialize_method,
                               CachedBoxedObjects{begin, end});
}

bool ImmutableAttributeAnalyzerState::is_jvm_cached_object(
    DexMethod* initialize_method, long value) const {
  auto it = cached_boxed_objects.find(initialize_method);
  if (it == cached_boxed_objects.end()) {
    return false;
  }
  auto& cached_objects = it->second;
  return value >= cached_objects.begin && value < cached_objects.end;
}

DexType* ImmutableAttributeAnalyzerState::initialized_type(
    const DexMethod* initialize_method) {
  return method::is_init(initialize_method)
             ? initialize_method->get_class()
             : initialize_method->get_proto()->get_rtype();
}

bool ImmutableAttributeAnalyzer::analyze_iget(
    const ImmutableAttributeAnalyzerState* state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  auto field_ref = insn->get_field();
  DexField* field = resolve_field(field_ref, FieldSearch::Instance);
  if (!field) {
    field = static_cast<DexField*>(field_ref);
  }
  if (!state->attribute_fields.count(field)) {
    return false;
  }
  auto this_domain = env->get(insn->src(0));
  if (this_domain.is_top() || this_domain.is_bottom()) {
    return false;
  }
  if (const auto& obj_or_none =
          this_domain.maybe_get<ObjectWithImmutAttrDomain>()) {
    auto object = obj_or_none->get_constant();
    auto value = object->get_value(field);
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
  } else {
    return false;
  }
}

bool ImmutableAttributeAnalyzer::analyze_invoke(
    const ImmutableAttributeAnalyzerState* state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  auto method_ref = insn->get_method();
  DexMethod* method = resolve_method(method_ref, opcode_to_search(insn));
  if (!method) {
    // Redex may run without sdk as input, so the method resolving may fail.
    // Example: Integer.valueOf(I) is an external method.
    method = static_cast<DexMethod*>(method_ref);
  }
  if (state->method_initializers.count(method)) {
    return analyze_method_initialization(state, insn, env, method);
  } else if (state->attribute_methods.count(method)) {
    return analyze_method_attr(state, insn, env, method);
  }
  return false;
}

/**
 * Propagate method return value if this method is a getter method of an
 * immutable field of an object. `Integer.intValue()` is a such method.
 */
bool ImmutableAttributeAnalyzer::analyze_method_attr(
    const ImmutableAttributeAnalyzerState* /* unused */,
    const IRInstruction* insn,
    ConstantEnvironment* env,
    DexMethod* method) {
  if (insn->srcs_size() != 1) {
    return false;
  }
  auto this_domain = env->get(insn->src(0));
  if (this_domain.is_top() || this_domain.is_bottom()) {
    return false;
  }
  if (const auto& obj_or_none =
          this_domain.maybe_get<ObjectWithImmutAttrDomain>()) {
    auto object = obj_or_none->get_constant();
    auto value = object->get_value(method);
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
  } else {
    return false;
  }
}

bool ImmutableAttributeAnalyzer::analyze_method_initialization(
    const ImmutableAttributeAnalyzerState* state,
    const IRInstruction* insn,
    ConstantEnvironment* env,
    DexMethod* method) {
  auto it = state->method_initializers.find(method);
  if (it == state->method_initializers.end()) {
    return false;
  }
  ObjectWithImmutAttr object(
      ImmutableAttributeAnalyzerState::initialized_type(method),
      it->second.size());
  // Only support one register for the object, can be easily extended. For
  // example, virtual method may return `this` pointer, so two registers are
  // holding the same heap object.
  reg_t obj_reg;
  bool has_value = false;
  for (auto& initializer : it->second) {
    obj_reg = initializer.obj_is_dest()
                  ? RESULT_REGISTER
                  : insn->src(*initializer.insn_src_id_of_obj);
    const auto& domain = env->get(insn->src(initializer.insn_src_id_of_attr));
    if (const auto& signed_value = domain.maybe_get<SignedConstantDomain>()) {
      auto constant = signed_value->get_constant();
      if (!constant) {
        object.write_value(initializer.attr, SignedConstantDomain::top());
        continue;
      }
      object.jvm_cached_singleton =
          state->is_jvm_cached_object(method, *constant);
      object.write_value(initializer.attr, *signed_value);
      has_value = true;
    } else if (const auto& string_value = domain.maybe_get<StringDomain>()) {
      if (!string_value->is_value()) {
        object.write_value(initializer.attr, StringDomain::top());
        continue;
      }
      object.write_value(initializer.attr, *string_value);
      has_value = true;
    } else if (const auto& type_value =
                   domain.maybe_get<ConstantClassObjectDomain>()) {
      if (!type_value->is_value()) {
        object.write_value(initializer.attr, ConstantClassObjectDomain::top());
        continue;
      }
      object.write_value(initializer.attr, *type_value);
      has_value = true;
    }
  }
  if (!has_value || object.empty()) {
    return false;
  }
  env->set(obj_reg, ObjectWithImmutAttrDomain(std::move(object)));
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
    auto last_insn = b->get_last_insn();
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      fp_iter.analyze_instruction(insn, &env, insn == last_insn->insn);
      if (opcode::is_a_return(insn->opcode())) {
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

bool EnumUtilsFieldAnalyzer::analyze_sget(
    ImmutableAttributeAnalyzerState* state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  // The $EnumUtils class contains fields named fXXX, where XXX encodes a 32-bit
  // number whose boxed value is stored as a java.lang.Integer instance in that
  // field. These fields are initialized through Integer.valueOf(...).
  auto integer_type = type::java_lang_Integer();
  auto field = resolve_field(insn->get_field());
  if (field == nullptr || !is_final(field) ||
      field->get_type() != integer_type || field->str().empty() ||
      field->str()[0] != 'f' ||
      field->get_class() != DexType::make_type("Lredex/$EnumUtils;")) {
    return false;
  }
  auto valueOf = method::java_lang_Integer_valueOf();
  auto it = state->method_initializers.find(valueOf);
  if (it == state->method_initializers.end()) {
    return false;
  }
  const auto& initializers = it->second;
  always_assert(initializers.size() == 1);
  const auto& initializer = initializers.front();
  always_assert(initializer.insn_src_id_of_attr == 0);

  const auto& name = field->str();
  auto value = std::stoi(name.substr(1));
  ObjectWithImmutAttr object(integer_type, 1);
  object.write_value(initializer.attr, SignedConstantDomain(value));
  object.jvm_cached_singleton = state->is_jvm_cached_object(valueOf, value);
  env->set(RESULT_REGISTER, ObjectWithImmutAttrDomain(std::move(object)));
  return true;
}

namespace intraprocedural {

namespace {

std::atomic<std::unordered_set<DexMethodRef*>*> kotlin_null_assertions{nullptr};
const std::unordered_set<DexMethodRef*>& get_kotlin_null_assertions() {
  for (;;) {
    auto* ptr = kotlin_null_assertions.load();
    if (ptr != nullptr) {
      return *ptr;
    }
    auto uptr = std::make_unique<std::unordered_set<DexMethodRef*>>(
        kotlin_nullcheck_wrapper::get_kotlin_null_assertions());
    if (kotlin_null_assertions.compare_exchange_strong(ptr, uptr.get())) {
      // Add a task to delete stuff.
      g_redex->add_destruction_task([]() {
        auto* ptr = kotlin_null_assertions.load();
        if (ptr != nullptr) {
          auto old_ptr = ptr;
          if (kotlin_null_assertions.compare_exchange_strong(ptr, nullptr)) {
            delete old_ptr;
          }
        }
      });
      return *uptr.release();
    }
  }
}

} // namespace

FixpointIterator::FixpointIterator(
    const cfg::ControlFlowGraph& cfg,
    InstructionAnalyzer<ConstantEnvironment> insn_analyzer,
    bool imprecise_switches)
    : MonotonicFixpointIterator(cfg),
      m_insn_analyzer(std::move(insn_analyzer)),
      m_kotlin_null_check_assertions(get_kotlin_null_assertions()),
      m_imprecise_switches(imprecise_switches) {}

void FixpointIterator::analyze_instruction(const IRInstruction* insn,
                                           ConstantEnvironment* env,
                                           bool is_last) const {
  TRACE(CONSTP, 5, "Analyzing instruction: %s", SHOW(insn));
  m_insn_analyzer(insn, env);
  if (!is_last) {
    analyze_instruction_no_throw(insn, env);
  }
}

void FixpointIterator::analyze_instruction_no_throw(
    const IRInstruction* insn, ConstantEnvironment* current_state) const {
  auto src_index = get_dereferenced_object_src_index(insn);
  if (!src_index) {
    src_index =
        get_null_check_object_index(insn, m_kotlin_null_check_assertions);
  }
  if (!src_index) {
    return;
  }
  if (insn->has_dest()) {
    auto dest = insn->dest();
    if ((dest == *src_index) ||
        (insn->dest_is_wide() && dest + 1 == *src_index)) {
      return;
    }
  }
  auto src = insn->src(*src_index);
  auto value = current_state->get(src);
  current_state->set(
      src, meet(value, SignedConstantDomain(sign_domain::Interval::NEZ)));
}

void FixpointIterator::analyze_node(const NodeId& block,
                                    ConstantEnvironment* state_at_entry) const {
  TRACE(CONSTP, 5, "Analyzing block: %zu", block->id());
  auto last_insn = block->get_last_insn();
  for (auto& mie : InstructionIterable(block)) {
    auto insn = mie.insn;
    analyze_instruction(insn, state_at_entry, insn == last_insn->insn);
  }
}

/*
 * Helpers for CFG edge analysis
 */

struct IfZeroMeetWith {
  sign_domain::Interval right_zero_meet_interval;
  boost::optional<sign_domain::Interval> left_zero_meet_interval{boost::none};
};

static const std::unordered_map<IROpcode, IfZeroMeetWith, boost::hash<IROpcode>>
    if_zero_meet_with{
        {OPCODE_IF_EQZ, {sign_domain::Interval::EQZ}},
        {OPCODE_IF_NEZ, {sign_domain::Interval::NEZ}},
        {OPCODE_IF_LTZ, {sign_domain::Interval::LTZ}},
        {OPCODE_IF_GTZ, {sign_domain::Interval::GTZ}},
        {OPCODE_IF_LEZ, {sign_domain::Interval::LEZ}},
        {OPCODE_IF_GEZ, {sign_domain::Interval::GEZ}},

        {OPCODE_IF_EQ,
         {sign_domain::Interval::EQZ, sign_domain::Interval::EQZ}},
        {OPCODE_IF_NE,
         {sign_domain::Interval::NEZ, sign_domain::Interval::NEZ}},
        {OPCODE_IF_LT,
         {sign_domain::Interval::LTZ, sign_domain::Interval::GTZ}},
        {OPCODE_IF_GT,
         {sign_domain::Interval::GTZ, sign_domain::Interval::LTZ}},
        {OPCODE_IF_LE,
         {sign_domain::Interval::LEZ, sign_domain::Interval::GEZ}},
        {OPCODE_IF_GE,
         {sign_domain::Interval::GEZ, sign_domain::Interval::LEZ}},
    };

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
  const IfZeroMeetWith& izmw = if_zero_meet_with.at(op);
  if (right == SignedConstantDomain(0)) {
    env->set(insn->src(0),
             meet(left, SignedConstantDomain(izmw.right_zero_meet_interval)));
    return;
  }
  if (left == SignedConstantDomain(0)) {
    env->set(insn->src(1),
             meet(right, SignedConstantDomain(*izmw.left_zero_meet_interval)));
    return;
  }

  switch (op) {
  case OPCODE_IF_EQ: {
    auto refined_value = meet(left, right);
    env->set(insn->src(0), refined_value);
    env->set(insn->src(1), refined_value);
    break;
  }
  case OPCODE_IF_NE: {
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
  case OPCODE_IF_GT: {
    if (ConstantValue::apply_visitor(runtime_leq_visitor(), left, right)) {
      env->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_LE: {
    if (ConstantValue::apply_visitor(runtime_lt_visitor(), right, left)) {
      env->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_GE: {
    if (ConstantValue::apply_visitor(runtime_lt_visitor(), left, right)) {
      env->set_to_bottom();
    }
    break;
  }
  default: {
    not_reached_log("expected if-* opcode, got %s", SHOW(insn));
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
  if (opcode::is_a_conditional_branch(op)) {
    analyze_if(insn, &env, edge->type() == cfg::EDGE_BRANCH);
  } else if (opcode::is_switch(op)) {
    auto selector_val = env.get(insn->src(0));
    const auto& case_key = edge->case_key();
    if (case_key) {
      always_assert(edge->type() == cfg::EDGE_BRANCH);
      selector_val.meet_with(SignedConstantDomain(*case_key));
      if (m_imprecise_switches) {
        // We could refine the selector value itself, for maximum knowledge.
        // However, in practice, this can cause following blocks to be refined
        // with the constant, which then degrades subsequent block deduping.
        if (selector_val.is_bottom()) {
          env.set_to_bottom();
          return env;
        }
      } else {
        env.set(insn->src(0), selector_val);
      }
    } else {
      always_assert(edge->type() == cfg::EDGE_GOTO);
      // We are looking at the fallthrough case. Set env to bottom in case there
      // is a non-fallthrough edge with a case-key that is equal to the actual
      // selector value.
      auto scd = selector_val.maybe_get<SignedConstantDomain>();
      auto selector_const = scd->get_constant();
      if (!selector_const) {
        return env;
      }
      auto succ = get_switch_succ(edge->src(), *selector_const);
      if (succ != nullptr) {
        const auto& succ_case_key = succ->case_key();
        always_assert(succ_case_key && *selector_const == *succ_case_key);
        env.set_to_bottom();
      }
    }
  } else if (edge->type() != cfg::EDGE_THROW) {
    analyze_instruction_no_throw(insn, &env);
  }
  return env;
}

} // namespace intraprocedural

} // namespace constant_propagation
