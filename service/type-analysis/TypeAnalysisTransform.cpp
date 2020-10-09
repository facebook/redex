/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisTransform.h"

#include "DexInstruction.h"
#include "Show.h"

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

void Transform::remove_redundant_null_checks(const DexTypeEnvironment& env,
                                             cfg::Block* block,
                                             Stats& stats) {
  auto insn_it = block->get_last_insn();
  if (insn_it == block->end()) {
    return;
  }
  auto last_insn = insn_it->insn;
  if (!opcode::is_a_testz_branch(last_insn->opcode())) {
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

void Transform::remove_redundant_type_checks(const DexTypeEnvironment& env,
                                             IRList::iterator& it,
                                             Stats& stats) {

  auto insn = it->insn;
  auto move_res = (++it)->insn;
  always_assert(insn->opcode() == OPCODE_INSTANCE_OF);
  always_assert(opcode::is_move_result_any(move_res->opcode()));
  auto val = env.get(insn->src(0));
  if (val.is_top() || val.is_bottom()) {
    return;
  }
  auto val_type = val.get_dex_type();
  if (val.is_null()) {
    // always 0
    auto eval_val = new IRInstruction(OPCODE_CONST);
    eval_val->set_literal(0)->set_dest(move_res->dest());
    m_replacements.push_back({insn, eval_val});
    stats.type_check_removed++;
  } else if (val.is_not_null() && val_type) {
    // can be evaluated
    DexType* src_type = const_cast<DexType*>(*val_type);
    auto eval_res = type::evaluate_type_check(src_type, insn->get_type());
    if (!eval_res) {
      return;
    }
    auto eval_val = new IRInstruction(OPCODE_CONST);
    eval_val->set_literal(*eval_res)->set_dest(move_res->dest());
    m_replacements.push_back({insn, eval_val});
    stats.type_check_removed++;
  } else if (val.is_nullable() && val_type) {
    // check can be converted to null checks
    DexType* src_type = const_cast<DexType*>(*val_type);
    auto eval_res = type::evaluate_type_check(src_type, insn->get_type());
    if (eval_res) {
      stats.null_check_only_type_checks++;
    }
  }
}

Transform::Stats Transform::apply(
    const type_analyzer::local::LocalTypeAnalyzer& lta,
    const WholeProgramState& wps,
    DexMethod* method,
    const NullAssertionSet& null_assertion_set) {
  auto code = method->get_code();
  TRACE(TYPE_TRANSFORM, 4, "Processing %s", SHOW(method));
  Transform::Stats stats{};
  bool can_use_nullness_results = wps.can_use_nullness_results(method);
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
      if (m_config.remove_redundant_type_checks && can_use_nullness_results &&
          insn->opcode() == OPCODE_INSTANCE_OF) {
        remove_redundant_type_checks(env, it, stats);
      }
    }
    if (m_config.remove_redundant_null_checks && can_use_nullness_results) {
      remove_redundant_null_checks(env, block, stats);
    }
  }
  apply_changes(method);
  return stats;
}

void Transform::apply_changes(DexMethod* method) {
  auto* code = method->get_code();
  for (auto const& p : m_replacements) {
    IRInstruction* old_op = p.first;
    if (opcode::is_branch(old_op->opcode())) {
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
