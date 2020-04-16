/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisTransform.h"

namespace type_analyzer {
constexpr const char* CHECK_PARAM_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/"
    "Object;Ljava/lang/String;)V";
constexpr const char* CHECK_EXPR_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/"
    "lang/Object;Ljava/lang/String;)V";

Transform::Stats Transform::apply(
    const type_analyzer::local::LocalTypeAnalyzer& lta,
    IRCode* code,
    const NullAssertionSet& null_assertion_set) {
  Transform::Stats stats{};
  for (const auto& block : code->cfg().blocks()) {
    auto env = lta.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      auto it = code->iterator_to(mie);
      auto* insn = mie.insn;
      lta.analyze_instruction(insn, &env);

      if (insn->opcode() == OPCODE_INVOKE_STATIC &&
          null_assertion_set.count(insn->get_method())) {

        auto parm = env.get(insn->src(0));
        if (parm.is_top() || parm.is_bottom()) {
          continue;
        }

        if (parm.is_not_null()) {
          m_deletes.emplace_back(it);
          stats.null_check_insn_removed++;
        }
      }
    }
  }
  apply_changes(code);
  return stats;
}

void Transform::apply_changes(IRCode* code) {
  for (const auto& it : m_deletes) {
    TRACE(TYPE_TRANSFORM, 4, "Removing instruction %s", SHOW(it->insn));
    code->remove_opcode(it);
  }
}

void Transform::setup(NullAssertionSet& null_assertion_set) {
  auto check_param_method = DexMethod::get_method(CHECK_PARAM_NULL_SIGNATURE);
  if (check_param_method) {
    null_assertion_set.insert(check_param_method);
  }
  auto check_expr_method = DexMethod::get_method(CHECK_EXPR_NULL_SIGNATURE);
  if (check_expr_method) {
    null_assertion_set.insert(check_expr_method);
  }
}

} // namespace type_analyzer
