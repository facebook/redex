/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalTypeAnalyzer.h"

#include <ostream>
#include <sstream>

namespace type_analyzer {

namespace local {

void set_dex_type(DexTypeEnvironment* state,
                  reg_t reg,
                  const boost::optional<const DexType*>& dex_type_opt) {
  const DexTypeDomain dex_type =
      dex_type_opt ? DexTypeDomain(*dex_type_opt) : DexTypeDomain::top();
  state->set(reg, dex_type);
}

boost::optional<const DexType*> get_dex_type(DexTypeEnvironment* state,
                                             reg_t reg) {
  return state->get(reg).get_dex_type();
}

void LocalTypeAnalyzer::analyze_instruction(const IRInstruction* insn,
                                            DexTypeEnvironment* env) const {
  TRACE(TYPE, 5, "Analyzing instruction: %s", SHOW(insn));
  m_insn_analyzer(insn, env);
}

bool InstructionTypeAnalyzer::analyze_load_param(
    const IRInstruction* insn, DexTypeEnvironment* /* unused */) {
  if (insn->opcode() != IOPCODE_LOAD_PARAM_OBJECT) {
    return false;
  }
  // TODO: Do stuff?
  return true;
}

bool InstructionTypeAnalyzer::analyze_move(const IRInstruction* insn,
                                           DexTypeEnvironment* env) {
  if (insn->opcode() != OPCODE_MOVE_OBJECT) {
    return false;
  }
  const auto& dex_type_opt = get_dex_type(env, insn->src(0));
  set_dex_type(env, insn->dest(), dex_type_opt);
  return true;
}

bool InstructionTypeAnalyzer::analyze_move_result(const IRInstruction* insn,
                                                  DexTypeEnvironment* env) {
  if (insn->opcode() != OPCODE_MOVE_RESULT_OBJECT ||
      insn->opcode() != IOPCODE_MOVE_RESULT_PSEUDO_OBJECT) {
    return false;
  }
  set_dex_type(env, insn->dest(), get_dex_type(env, RESULT_REGISTER));
  return true;
}

bool InstructionTypeAnalyzer::analyze_move_exception(const IRInstruction* insn,
                                                     DexTypeEnvironment* env) {
  // We don't know where to grab the type of the just-caught exception.
  // Simply set to j.l.Throwable here.
  set_dex_type(env, insn->dest(), type::java_lang_Throwable());
  return true;
}

bool InstructionTypeAnalyzer::analyze_new_instance(const IRInstruction* insn,
                                                   DexTypeEnvironment* env) {
  set_dex_type(env, RESULT_REGISTER, insn->get_type());
  return true;
}

bool InstructionTypeAnalyzer::analyze_new_array(const IRInstruction* insn,
                                                DexTypeEnvironment* env) {
  set_dex_type(env, RESULT_REGISTER, insn->get_type());
  return true;
}

bool InstructionTypeAnalyzer::analyze_filled_new_array(
    const IRInstruction* insn, DexTypeEnvironment* env) {
  set_dex_type(env, RESULT_REGISTER, insn->get_type());
  return true;
}

} // namespace local

} // namespace type_analyzer
