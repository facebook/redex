/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalTypeAnalyzer.h"

#include <ostream>
#include <sstream>

#include "Resolver.h"
#include "Show.h"
#include "Trace.h"

using namespace type_analyzer;

namespace type_analyzer {

namespace local {

/*
 * For debugging only
 * void traceEnvironment(DexTypeEnvironment* env) {
 *   std::ostringstream out;
 *   out << *env;
 *   TRACE(TYPE, 9, "%s", out.str().c_str());
 * }
 */

void LocalTypeAnalyzer::analyze_instruction(const IRInstruction* insn,
                                            DexTypeEnvironment* env) const {
  TRACE(TYPE, 9, "Analyzing instruction: %s", SHOW(insn));
  m_insn_analyzer(insn, env);
}

bool RegisterTypeAnalyzer::analyze_default(const IRInstruction* insn,
                                           DexTypeEnvironment* env) {
  if (opcode::is_a_load_param(insn->opcode())) {
    return true;
  }
  if (insn->has_dest()) {
    env->set(insn->dest(), DexTypeDomain::top());
    if (insn->dest_is_wide()) {
      env->set(insn->dest() + 1, DexTypeDomain::top());
    }
  } else if (insn->has_move_result_any()) {
    env->set(RESULT_REGISTER, DexTypeDomain::top());
  }
  return true;
}

bool RegisterTypeAnalyzer::analyze_const(const IRInstruction* insn,
                                         DexTypeEnvironment* env) {
  if (insn->opcode() != OPCODE_CONST) {
    return false;
  }
  if (insn->get_literal() == 0) {
    env->set(insn->dest(), DexTypeDomain::null());
  } else {
    env->set(insn->dest(), DexTypeDomain(insn->get_literal()));
  }
  return true;
}

bool RegisterTypeAnalyzer::analyze_const_string(const IRInstruction*,
                                                DexTypeEnvironment* env) {
  env->set(RESULT_REGISTER, DexTypeDomain(type::java_lang_String()));
  return true;
}

bool RegisterTypeAnalyzer::analyze_const_class(const IRInstruction*,
                                               DexTypeEnvironment* env) {
  env->set(RESULT_REGISTER, DexTypeDomain(type::java_lang_Class()));
  return true;
}

bool RegisterTypeAnalyzer::analyze_aget(const IRInstruction* insn,
                                        DexTypeEnvironment* env) {
  auto array_type = env->get(insn->src(0)).get_dex_type();
  if (!array_type || !*array_type) {
    env->set(RESULT_REGISTER, DexTypeDomain::top());
    return true;
  }

  always_assert_log(type::is_array(*array_type), "Wrong array type %s",
                    SHOW(*array_type));
  auto idx_opt = env->get(insn->src(1)).get_constant();
  auto nullness = env->get(insn->src(0)).get_array_element_nullness(idx_opt);
  const auto ctype = type::get_array_component_type(*array_type);
  env->set(RESULT_REGISTER, DexTypeDomain(ctype, nullness.element()));
  return true;
}

/*
 * Only populating array nullness since we don't model array element types.
 */
bool RegisterTypeAnalyzer::analyze_aput(const IRInstruction* insn,
                                        DexTypeEnvironment* env) {
  if (insn->opcode() != OPCODE_APUT_OBJECT) {
    return false;
  }
  boost::optional<int64_t> idx_opt = env->get(insn->src(2)).get_constant();
  auto nullness = env->get(insn->src(0)).get_nullness();
  env->mutate_reg_environment([&](RegTypeEnvironment* env) {
    auto array_reg = insn->src(1);
    env->update(array_reg, [&](const DexTypeDomain& domain) {
      auto copy = domain;
      copy.set_array_element_nullness(idx_opt, nullness);
      return copy;
    });
  });
  return true;
}

bool RegisterTypeAnalyzer::analyze_array_length(const IRInstruction* insn,
                                                DexTypeEnvironment* env) {
  auto array_nullness = env->get(insn->src(0)).get_array_nullness();
  if (array_nullness.is_top()) {
    env->set(RESULT_REGISTER, DexTypeDomain::top());
    return true;
  }
  if (auto array_length = array_nullness.get_length()) {
    env->set(RESULT_REGISTER, DexTypeDomain(*array_length));
  } else {
    env->set(RESULT_REGISTER, DexTypeDomain::top());
  }
  return true;
}

bool RegisterTypeAnalyzer::analyze_binop_lit(const IRInstruction* insn,
                                             DexTypeEnvironment* env) {
  auto op = insn->opcode();
  int32_t lit = insn->get_literal();
  auto int_val = env->get(insn->src(0)).get_constant();
  boost::optional<int64_t> result = boost::none;

  if (!int_val) {
    return analyze_default(insn, env);
  }

  bool use_result_reg = false;
  switch (op) {
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_ADD_INT_LIT8: {
    result = (*int_val) + lit;
    break;
  }
  case OPCODE_RSUB_INT:
  case OPCODE_RSUB_INT_LIT8: {
    result = lit - (*int_val);
    break;
  }
  case OPCODE_MUL_INT_LIT16:
  case OPCODE_MUL_INT_LIT8: {
    result = (*int_val) * lit;
    break;
  }
  case OPCODE_DIV_INT_LIT16:
  case OPCODE_DIV_INT_LIT8: {
    if (lit != 0) {
      result = (*int_val) / lit;
    }
    use_result_reg = true;
    break;
  }
  case OPCODE_REM_INT_LIT16:
  case OPCODE_REM_INT_LIT8: {
    if (lit != 0) {
      result = (*int_val) % lit;
    }
    use_result_reg = true;
    break;
  }
  case OPCODE_AND_INT_LIT16:
  case OPCODE_AND_INT_LIT8: {
    result = (*int_val) & lit;
    break;
  }
  case OPCODE_OR_INT_LIT16:
  case OPCODE_OR_INT_LIT8: {
    result = (*int_val) | lit;
    break;
  }
  case OPCODE_XOR_INT_LIT16:
  case OPCODE_XOR_INT_LIT8: {
    result = (*int_val) ^ lit;
    break;
  }
  // as in https://source.android.com/devices/tech/dalvik/dalvik-bytecode
  // the following operations have the second operand masked.
  case OPCODE_SHL_INT_LIT8: {
    uint32_t ucst = *int_val;
    uint32_t uresult = ucst << (lit & 0x1f);
    result = (int32_t)uresult;
    break;
  }
  case OPCODE_SHR_INT_LIT8: {
    result = (*int_val) >> (lit & 0x1f);
    break;
  }
  case OPCODE_USHR_INT_LIT8: {
    uint32_t ucst = *int_val;
    // defined in dalvik spec
    result = ucst >> (lit & 0x1f);
    break;
  }
  default:
    break;
  }
  auto res_dom = DexTypeDomain::top();
  if (result != boost::none) {
    int32_t result32 = (int32_t)(*result & 0xFFFFFFFF);
    res_dom = DexTypeDomain(result32);
  }
  env->set(use_result_reg ? RESULT_REGISTER : insn->dest(), res_dom);
  return true;
}

bool RegisterTypeAnalyzer::analyze_binop(const IRInstruction* insn,
                                         DexTypeEnvironment* env) {
  auto op = insn->opcode();
  auto int_left = env->get(insn->src(0)).get_constant();
  auto int_right = env->get(insn->src(1)).get_constant();
  if (!int_left || !int_right) {
    return analyze_default(insn, env);
  }

  boost::optional<int64_t> result = boost::none;
  bool use_result_reg = false;
  switch (op) {
  case OPCODE_ADD_INT:
  case OPCODE_ADD_LONG: {
    result = (*int_left) + (*int_right);
    break;
  }
  case OPCODE_SUB_INT:
  case OPCODE_SUB_LONG: {
    result = (*int_left) - (*int_right);
    break;
  }
  case OPCODE_MUL_INT:
  case OPCODE_MUL_LONG: {
    result = (*int_left) * (*int_right);
    break;
  }
  case OPCODE_DIV_INT:
  case OPCODE_DIV_LONG: {
    if ((*int_right) != 0) {
      result = (*int_left) / (*int_right);
    }
    use_result_reg = true;
    break;
  }
  case OPCODE_REM_INT:
  case OPCODE_REM_LONG: {
    if ((*int_right) != 0) {
      result = (*int_left) % (*int_right);
    }
    use_result_reg = true;
    break;
  }
  case OPCODE_AND_INT:
  case OPCODE_AND_LONG: {
    result = (*int_left) & (*int_right);
    break;
  }
  case OPCODE_OR_INT:
  case OPCODE_OR_LONG: {
    result = (*int_left) | (*int_right);
    break;
  }
  case OPCODE_XOR_INT:
  case OPCODE_XOR_LONG: {
    result = (*int_left) ^ (*int_right);
    break;
  }
  default:
    return analyze_default(insn, env);
  }
  auto res_dom = DexTypeDomain::top();
  if (result != boost::none) {
    if (opcode::is_binop64(op)) {
      res_dom = DexTypeDomain(*result);
    } else {
      int32_t result32 = (int32_t)(*result & 0xFFFFFFFF);
      res_dom = DexTypeDomain(result32);
    }
  }
  env->set(use_result_reg ? RESULT_REGISTER : insn->dest(), res_dom);
  return true;
}

bool RegisterTypeAnalyzer::analyze_move(const IRInstruction* insn,
                                        DexTypeEnvironment* env) {
  env->set(insn->dest(), env->get(insn->src(0)));
  return true;
}

bool RegisterTypeAnalyzer::analyze_move_result(const IRInstruction* insn,
                                               DexTypeEnvironment* env) {
  env->set(insn->dest(), env->get(RESULT_REGISTER));
  return true;
}

bool RegisterTypeAnalyzer::analyze_move_exception(const IRInstruction* insn,
                                                  DexTypeEnvironment* env) {
  // We don't know where to grab the type of the just-caught exception.
  // Simply set to j.l.Throwable here.
  env->set(insn->dest(), DexTypeDomain(type::java_lang_Throwable()));
  return true;
}

bool RegisterTypeAnalyzer::analyze_new_instance(const IRInstruction* insn,
                                                DexTypeEnvironment* env) {
  env->set(RESULT_REGISTER, DexTypeDomain(insn->get_type()));
  return true;
}

bool RegisterTypeAnalyzer::analyze_new_array(const IRInstruction* insn,
                                             DexTypeEnvironment* env) {
  // Skip array element nullness domains.
  env->set(RESULT_REGISTER, DexTypeDomain(insn->get_type()));
  return true;
}

bool RegisterTypeAnalyzer::analyze_filled_new_array(const IRInstruction* insn,
                                                    DexTypeEnvironment* env) {
  // TODO(zwei): proper array nullness domain population.
  env->set(RESULT_REGISTER, DexTypeDomain(insn->get_type(), 0));
  return true;
}

bool RegisterTypeAnalyzer::analyze_invoke(const IRInstruction* insn,
                                          DexTypeEnvironment* env) {
  auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (method == nullptr) {
    return analyze_default(insn, env);
  }
  // Note we don't need to take care of the RESULT_REGISTER update from this
  // point. The remaining cases are already taken care by the
  // WholeProgramAwareAnalyzer::analyze_invoke.
  //
  // When passed through a call, we need to reset the elements of an
  // ArrayNullnessDomain. The domain passed to the callee is a copy and can be
  // written over there. That means that the local ArrayNullnessDomain stored in
  // the caller environment might be out of date.
  //
  // E.g., a newly allocated array in a caller environment has its elements
  // initially as UNINITIALIED. The array elements can be updated by a callee
  // which has access to the array. At that point, the updated element is no
  // longer UNINITIALIED. However, the change is not propagated to the caller
  // environment. Reference: T107422148, T123970364
  for (auto src : insn->srcs()) {
    auto type_domain = env->get(src);
    auto array_nullness = type_domain.get_array_nullness();
    auto dex_type = type_domain.get_dex_type();

    if (!array_nullness.is_top() && array_nullness.get_length() &&
        *array_nullness.get_length() > 0 && dex_type) {
      env->mutate_reg_environment([&](RegTypeEnvironment* env) {
        env->map([&](const DexTypeDomain& domain) {
          auto dex_type_local = domain.get_dex_type();
          if (dex_type_local && *dex_type == *dex_type_local) {
            return DexTypeDomain(*dex_type_local,
                                 domain.get_nullness().element());
          }
          return domain;
        });
      });
    }
  }
  return false;
}

namespace {

bool field_get_helper(const DexType* class_under_init,
                      const IRInstruction* insn,
                      DexTypeEnvironment* env) {
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

bool field_put_helper(const DexType* class_under_init,
                      const IRInstruction* insn,
                      DexTypeEnvironment* env) {
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

} // namespace

bool ClinitFieldAnalyzer::analyze_sget(const DexType* class_under_init,
                                       const IRInstruction* insn,
                                       DexTypeEnvironment* env) {
  return field_get_helper(class_under_init, insn, env);
}

bool ClinitFieldAnalyzer::analyze_sput(const DexType* class_under_init,
                                       const IRInstruction* insn,
                                       DexTypeEnvironment* env) {
  return field_put_helper(class_under_init, insn, env);
}

bool CtorFieldAnalyzer::analyze_default(const DexType* class_under_init,
                                        const IRInstruction* insn,
                                        DexTypeEnvironment* env) {
  if (!class_under_init) {
    return false;
  }
  if (insn->has_dest()) {
    env->set_this_ptr(insn->dest(), IsDomain::top());
    if (insn->dest_is_wide()) {
      env->set_this_ptr(insn->dest() + 1, IsDomain::top());
    }
  } else if (insn->has_move_result_any()) {
    env->set_this_ptr(RESULT_REGISTER, IsDomain::top());
  }
  return false;
}

bool CtorFieldAnalyzer::analyze_load_param(const DexType* class_under_init,
                                           const IRInstruction* insn,
                                           DexTypeEnvironment* env) {
  if (!class_under_init || insn->opcode() != IOPCODE_LOAD_PARAM_OBJECT) {
    return false;
  }
  if (env->get_this_ptr_environment().is_top()) {
    env->set_this_ptr(insn->dest(), IsDomain(true));
  }
  return false;
}

bool CtorFieldAnalyzer::analyze_iget(const DexType* class_under_init,
                                     const IRInstruction* insn,
                                     DexTypeEnvironment* env) {
  if (!env->is_this_ptr(insn->src(0))) {
    return false;
  }
  return field_get_helper(class_under_init, insn, env);
}

bool CtorFieldAnalyzer::analyze_iput(const DexType* class_under_init,
                                     const IRInstruction* insn,
                                     DexTypeEnvironment* env) {
  if (!env->is_this_ptr(insn->src(1))) {
    return false;
  }
  return field_put_helper(class_under_init, insn, env);
}

bool CtorFieldAnalyzer::analyze_move(const DexType* class_under_init,
                                     const IRInstruction* insn,
                                     DexTypeEnvironment* env) {
  if (!class_under_init) {
    return false;
  }
  env->set_this_ptr(insn->dest(), env->get_this_ptr(insn->src(0)));
  return false;
}

bool CtorFieldAnalyzer::analyze_move_result(const DexType* class_under_init,
                                            const IRInstruction* insn,
                                            DexTypeEnvironment* env) {
  if (!class_under_init) {
    return false;
  }
  env->set_this_ptr(insn->dest(), env->get_this_ptr(RESULT_REGISTER));
  return false;
}

} // namespace local

} // namespace type_analyzer
