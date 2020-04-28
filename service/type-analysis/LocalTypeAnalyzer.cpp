/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalTypeAnalyzer.h"

#include <ostream>
#include <sstream>

#include "Resolver.h"

using namespace type_analyzer;

namespace {

bool field_get_helper(std::unordered_set<DexField*>* written_fields,
                      const IRInstruction* insn,
                      DexTypeEnvironment* env) {
  auto field = resolve_field(insn->get_field());
  if (field == nullptr || !type::is_object(field->get_type())) {
    return false;
  }
  env->set(RESULT_REGISTER, env->get(field));
  return true;
}

bool field_put_helper(std::unordered_set<DexField*>* written_fields,
                      const IRInstruction* insn,
                      DexTypeEnvironment* env) {
  auto field = resolve_field(insn->get_field());
  if (field == nullptr || !type::is_object(field->get_type())) {
    return false;
  }
  if (written_fields->count(field)) {
    // Has either been written to locally or by another method.
    auto temp_type = env->get(field);
    temp_type.join_with(env->get(insn->src(0)));
    env->set(field, temp_type);
  } else {
    env->set(field, env->get(insn->src(0)));
  }
  return true;
}

} // namespace

namespace type_analyzer {

namespace local {

void traceEnvironment(DexTypeEnvironment* env) {
  std::ostringstream out;
  out << *env;
  TRACE(TYPE, 9, "%s", out.str().c_str());
}

void LocalTypeAnalyzer::analyze_instruction(const IRInstruction* insn,
                                            DexTypeEnvironment* env) const {
  TRACE(TYPE, 9, "Analyzing instruction: %s", SHOW(insn));
  m_insn_analyzer(insn, env);
  if (traceEnabled(TYPE, 9)) {
    traceEnvironment(env);
  }
}

bool RegisterTypeAnalyzer::analyze_const(const IRInstruction* insn,
                                         DexTypeEnvironment* env) {
  if (insn->opcode() != OPCODE_CONST || insn->get_literal() != 0) {
    return false;
  }
  env->set(insn->dest(), DexTypeDomain::null());
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
  if (insn->opcode() != OPCODE_AGET_OBJECT) {
    return false;
  }
  auto array_type = env->get(insn->src(0)).get_dex_type();
  if (array_type && *array_type) {
    always_assert_log(
        type::is_array(*array_type), "Wrong array type %s", SHOW(*array_type));
    const auto ctype = type::get_array_component_type(*array_type);
    env->set(RESULT_REGISTER, DexTypeDomain(ctype));
  } else {
    env->set(RESULT_REGISTER, DexTypeDomain::top());
  }
  return true;
}

bool RegisterTypeAnalyzer::analyze_move(const IRInstruction* insn,
                                        DexTypeEnvironment* env) {
  if (insn->opcode() != OPCODE_MOVE_OBJECT) {
    return false;
  }
  env->set(insn->dest(), env->get(insn->src(0)));
  return true;
}

bool RegisterTypeAnalyzer::analyze_move_result(const IRInstruction* insn,
                                               DexTypeEnvironment* env) {
  if (insn->opcode() != OPCODE_MOVE_RESULT_OBJECT &&
      insn->opcode() != IOPCODE_MOVE_RESULT_PSEUDO_OBJECT) {
    return false;
  }
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
  env->set(RESULT_REGISTER, DexTypeDomain(insn->get_type()));
  return true;
}

bool RegisterTypeAnalyzer::analyze_filled_new_array(const IRInstruction* insn,
                                                    DexTypeEnvironment* env) {
  env->set(RESULT_REGISTER, DexTypeDomain(insn->get_type()));
  return true;
}

bool FieldTypeAnalyzer::analyze_iget(
    std::unordered_set<DexField*>* written_fields,
    const IRInstruction* insn,
    DexTypeEnvironment* env) {
  return field_get_helper(written_fields, insn, env);
}

bool FieldTypeAnalyzer::analyze_iput(
    std::unordered_set<DexField*>* written_fields,
    const IRInstruction* insn,
    DexTypeEnvironment* env) {
  return field_put_helper(written_fields, insn, env);
}

bool FieldTypeAnalyzer::analyze_sget(
    std::unordered_set<DexField*>* written_fields,
    const IRInstruction* insn,
    DexTypeEnvironment* env) {
  return field_get_helper(written_fields, insn, env);
}

bool FieldTypeAnalyzer::analyze_sput(
    std::unordered_set<DexField*>* written_fields,
    const IRInstruction* insn,
    DexTypeEnvironment* env) {
  return field_put_helper(written_fields, insn, env);
}

} // namespace local

} // namespace type_analyzer
