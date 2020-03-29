/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisTransform.h"

namespace type_analyzer {
constexpr const char* KOTLIN_INTRINSIC_CLASS =
    "Lkotlin/jvm/internal/Intrinsics;";
constexpr const char* CHECK_PARAM_NULL_METHOD = "checkParameterIsNotNull";
constexpr const char* CHECK_EXPR_NULL_METHOD = "checkExpressionValueIsNotNull";

Transform::Stats Transform::apply(
    const type_analyzer::local::LocalTypeAnalyzer& lta, IRCode* code) {
  for (const auto& block : code->cfg().blocks()) {
    auto env = lta.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      auto it = code->iterator_to(mie);
      auto* insn = mie.insn;
      lta.analyze_instruction(insn, &env);
      if (insn->opcode() != OPCODE_INVOKE_STATIC) {
        continue;
      }
      if (insn->get_method()->get_class()->get_name()->str() !=
          KOTLIN_INTRINSIC_CLASS) {
        continue;
      }
      if (insn->get_method()->get_name()->str() != CHECK_PARAM_NULL_METHOD &&
          insn->get_method()->get_name()->str() != CHECK_EXPR_NULL_METHOD) {
        continue;
      }
      auto parm = env.get(insn->src(0));
      if (parm.is_top() || parm.is_bottom()) {
        continue;
      }
      if (parm.is_not_null()) {
        m_deletes.emplace_back(it);
        m_stats.null_check_insn_removed++;
      }
    }
  }
  apply_changes(code);
  return m_stats;
}

void Transform::apply_changes(IRCode* code) {
  for (const auto& it : m_deletes) {
    TRACE(TYPE_TRANSFORM, 4, "Removing instruction %s", SHOW(it->insn));
    code->remove_opcode(it);
  }
}
} // namespace type_analyzer
