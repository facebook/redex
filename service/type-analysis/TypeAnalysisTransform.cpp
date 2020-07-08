/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisTransform.h"

#include "DexInstruction.h"

namespace {

constexpr const char* CHECK_PARAM_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/"
    "Object;Ljava/lang/String;)V";
constexpr const char* CHECK_EXPR_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/"
    "lang/Object;Ljava/lang/String;)V";

enum BranchResult {
  ALWAYS_TAKEN,
  NEVER_TAKEN,
  UNKNOWN,
};

/*
 * When constructing a ConstNullnessDomain for a constant value, NOT_NULL only
 * means that it's not zero. The value could be negative or positive.
 * Therefore, any signness test on a NOT_NULL is UNKNOWN.
 */
struct TestZeroNullnessResults {
  BranchResult is_null_result;
  BranchResult not_null_result;
};

/*
 * Null check on a reference type value will only be compiled to IF_EQZ or
 * IF_NEZ.
 * Since we do propagate constant values, we should be able to cover other
 * branch types. However, IPCP presumably already covers these constant cases.
 * That's why we just put UNKNOWN for other branch types. We can revisit this
 * later.
 * TODO: cover other branch type for constant values.
 */
static const std::
    unordered_map<IROpcode, TestZeroNullnessResults, boost::hash<IROpcode>>
        test_zero_results{
            {OPCODE_IF_EQZ, {ALWAYS_TAKEN, NEVER_TAKEN}},
            {OPCODE_IF_NEZ, {NEVER_TAKEN, ALWAYS_TAKEN}},
            {OPCODE_IF_LTZ, {UNKNOWN, UNKNOWN}},
            {OPCODE_IF_GTZ, {UNKNOWN, UNKNOWN}},
            {OPCODE_IF_LEZ, {UNKNOWN, UNKNOWN}},
            {OPCODE_IF_GEZ, {UNKNOWN, UNKNOWN}},
        };

BranchResult evaluate_branch(IROpcode op, Nullness operand_nullness) {
  always_assert(operand_nullness != NN_BOTTOM);
  const auto& branch_results = test_zero_results.at(op);
  if (operand_nullness == IS_NULL) {
    return branch_results.is_null_result;
  } else if (operand_nullness == NOT_NULL) {
    return branch_results.not_null_result;
  }
  return UNKNOWN;
}

bool is_supported_branch_type(IROpcode op) {
  return op == OPCODE_IF_EQZ || op == OPCODE_IF_NEZ;
}

} // namespace

namespace type_analyzer {

/*
 * The nullness results is only guaranteed to be correct after the execution of
 * clinit and ctors.
 * TODO: The complete solution requires some kind of call graph analysis from
 * the clinit and ctor.
 */
bool Transform::can_optimize_null_checks(const WholeProgramState& wps,
                                         const DexMethod* method) {
  return m_config.remove_redundant_null_checks && !method::is_init(method) &&
         !method::is_clinit(method) && !wps.is_any_init_reachable(method);
}

void Transform::remove_redundant_null_checks(const DexTypeEnvironment& env,
                                             cfg::Block* block,
                                             Stats& stats) {
  auto insn_it = block->get_last_insn();
  if (insn_it == block->end()) {
    return;
  }
  auto last_insn = insn_it->insn;
  if (!is_testz_branch(last_insn->opcode())) {
    return;
  }
  auto domain = env.get(last_insn->src(0));
  if (domain.is_bottom() || domain.is_nullable()) {
    return;
  }
  auto result =
      evaluate_branch(last_insn->opcode(), domain.get_nullness().element());
  if (result == ALWAYS_TAKEN) {
    m_replacements.push_back({last_insn, new IRInstruction(OPCODE_GOTO)});
    stats.null_check_removed++;
  } else if (result == NEVER_TAKEN) {
    m_deletes.emplace_back(insn_it);
    stats.null_check_removed++;
  } else if (!is_supported_branch_type(last_insn->opcode())) {
    stats.unsupported_branch++;
  }
}

Transform::Stats Transform::apply(
    const type_analyzer::local::LocalTypeAnalyzer& lta,
    const WholeProgramState& wps,
    DexMethod* method,
    const NullAssertionSet& null_assertion_set) {
  auto code = method->get_code();
  Transform::Stats stats{};
  bool remove_null_checks = can_optimize_null_checks(wps, method);
  for (const auto& block : code->cfg().blocks()) {
    auto env = lta.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      auto it = code->iterator_to(mie);
      auto* insn = mie.insn;
      lta.analyze_instruction(insn, &env);

      if (m_config.remove_kotlin_null_check_assertions &&
          insn->opcode() == OPCODE_INVOKE_STATIC &&
          null_assertion_set.count(insn->get_method())) {

        auto parm = env.get(insn->src(0));
        if (parm.is_top() || parm.is_bottom()) {
          continue;
        }

        if (parm.is_not_null()) {
          m_deletes.emplace_back(it);
          stats.kotlin_null_check_removed++;
        }
      }
    }

    if (remove_null_checks) {
      remove_redundant_null_checks(env, block, stats);
    }
  }
  apply_changes(code);
  return stats;
}

void Transform::apply_changes(IRCode* code) {
  for (auto const& p : m_replacements) {
    IRInstruction* old_op = p.first;
    if (is_branch(old_op->opcode())) {
      code->replace_branch(old_op, p.second);
    } else {
      code->replace_opcode(old_op, p.second);
    }
  }
  for (const auto& it : m_deletes) {
    TRACE(TYPE_TRANSFORM, 9, "Removing instruction %s", SHOW(it->insn));
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
