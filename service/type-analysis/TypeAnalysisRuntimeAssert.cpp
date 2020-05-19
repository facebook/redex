/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisRuntimeAssert.h"

#include "ProguardMap.h"
#include "Resolver.h"
#include "Walkers.h"

namespace type_analyzer {

RuntimeAssertTransform::Config::Config(const ProguardMap& pg_map) {
  param_assert_fail_handler = DexMethod::get_method(pg_map.translate_method(
      "Lcom/facebook/redex/"
      "ConstantPropagationAssertHandler;.paramValueError:(I)V"));
  field_assert_fail_handler = DexMethod::get_method(
      pg_map.translate_method("Lcom/facebook/redex/"
                              "ConstantPropagationAssertHandler;."
                              "fieldValueError:(Ljava/lang/String;)V"));
  return_value_assert_fail_handler = DexMethod::get_method(
      pg_map.translate_method("Lcom/facebook/redex/"
                              "ConstantPropagationAssertHandler;."
                              "returnValueError:(Ljava/lang/String;)V"));
}

static IRList::iterator insert_null_check(IRCode* code,
                                          IRList::iterator it,
                                          reg_t reg_to_check) {
  it = code->insert_after(
      it, (new IRInstruction(OPCODE_IF_EQZ))->set_src(0, reg_to_check));
  return it;
}

static IRList::iterator insert_type_check(IRCode* code,
                                          IRList::iterator it,
                                          reg_t reg_to_check,
                                          const DexType* dex_type) {
  always_assert(dex_type);
  auto res_reg = code->allocate_temp();
  it = code->insert_after(it,
                          (new IRInstruction(OPCODE_INSTANCE_OF))
                              ->set_type(const_cast<DexType*>(dex_type))
                              ->set_src(0, reg_to_check));
  it = code->insert_after(
      it, (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO))->set_dest(res_reg));
  it = code->insert_after(
      it, (new IRInstruction(OPCODE_IF_NEZ))->set_src(0, res_reg));
  return it;
}

template <typename DexMember>
static IRList::iterator insert_throw_error(IRCode* code,
                                           IRList::iterator it,
                                           const DexMember* member,
                                           DexMethodRef* handler) {
  auto member_name_reg = code->allocate_temp();
  it = code->insert_after(it,
                          ((new IRInstruction(OPCODE_CONST_STRING))
                               ->set_string(DexString::make_string(
                                   member->get_deobfuscated_name()))));
  it =
      code->insert_after(it,
                         ((new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                              ->set_dest(member_name_reg)));
  it = code->insert_after(it,
                          ((new IRInstruction(OPCODE_INVOKE_STATIC))
                               ->set_method(handler)
                               ->set_srcs_size(1)
                               ->set_src(0, member_name_reg)));
  return it;
}

// The logic here is designed for testing purpose. It is not designed for
// production settings. So the rules are more relaxed.
bool can_access(const DexType* from, const DexType* to) {
  always_assert(from && to);
  auto to_cls = type_class(to);
  if (!to_cls || is_public(to_cls) || from == to) {
    return true;
  }
  if (is_private(to_cls)) {
    return false;
  }
  auto is_same_package = type::same_package(from, to);
  if (is_package_private(to_cls) && is_same_package) {
    return true;
  }
  return false;
}

RuntimeAssertTransform::Stats RuntimeAssertTransform::apply(
    const local::LocalTypeAnalyzer& /* lta */,
    const WholeProgramState& wps,
    DexMethod* method) {
  auto* code = method->get_code();
  always_assert(code != nullptr);
  RuntimeAssertTransform::Stats stats{};
  bool in_try = false;
  for (auto it = code->begin(); it != code->end(); ++it) {
    // Avoid emitting checks in a try section.
    // The inserted checks could introduce a throw edge from a block in the try
    // section to the catch section. This could change the CFG and the data
    // flow, which could introduce a type violation.
    if (it->type == MFLOW_TRY) {
      if (it->tentry->type == TRY_START) {
        in_try = true;
      } else if (it->tentry->type == TRY_END) {
        in_try = false;
      }
      continue;
    } else if (it->type != MFLOW_OPCODE) {
      continue;
    }
    if (!in_try) {
      always_assert(it->insn);
      it = insert_field_assert(wps, method->get_class(), code, it, stats);
      it =
          insert_return_value_assert(wps, method->get_class(), code, it, stats);
    }
  }
  return stats;
}

IRList::iterator RuntimeAssertTransform::insert_field_assert(
    const WholeProgramState& wps,
    const DexType* from,
    IRCode* code,
    IRList::iterator& it,
    Stats& stats) {
  auto* insn = it->insn;
  auto op = insn->opcode();
  if (!is_sget(op) && !is_iget(op)) {
    return it;
  }
  auto* field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return it;
  }
  if (!type::is_object(field->get_type())) {
    return it;
  }
  auto dex_type = wps.get_field_type(field).get_dex_type();
  if (!dex_type) {
    return it;
  }
  if (!can_access(from, *dex_type)) {
    return it;
  }
  auto fm_it = it;
  auto reg_to_check = ir_list::move_result_pseudo_of(fm_it)->dest();
  ++fm_it; // skip the move-result-pseudo
  fm_it = insert_null_check(code, fm_it, reg_to_check);
  auto null_check_insn_it = fm_it;
  fm_it = insert_type_check(code, fm_it, reg_to_check, *dex_type);
  auto type_check_insn_it = fm_it;
  // Fall through to throw Error
  fm_it = insert_throw_error(code, fm_it, field,
                             m_config.field_assert_fail_handler);
  fm_it = code->insert_after(fm_it, new BranchTarget(&*null_check_insn_it));
  fm_it = code->insert_after(fm_it, new BranchTarget(&*type_check_insn_it));
  stats.field_type_check_inserted++;
  return fm_it;
}

IRList::iterator RuntimeAssertTransform::insert_return_value_assert(
    const WholeProgramState& wps,
    const DexType* from,
    IRCode* code,
    IRList::iterator& it,
    Stats& stats) {
  auto* insn = it->insn;
  if (!is_invoke(insn->opcode())) {
    return it;
  }
  auto* callee = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (callee == nullptr) {
    return it;
  }
  auto ret_type = callee->get_proto()->get_rtype();
  if (!type::is_object(ret_type)) {
    return it;
  }
  auto dex_type = wps.get_return_type(callee).get_dex_type();
  if (!dex_type) {
    return it;
  }
  if (!can_access(from, *dex_type)) {
    return it;
  }
  ++it;
  if (!opcode::is_move_result(it->insn->opcode())) {
    return it;
  }
  auto reg_to_check = it->insn->dest();
  auto fm_it = it;
  fm_it = insert_null_check(code, fm_it, reg_to_check);
  auto null_check_insn_it = fm_it;
  fm_it = insert_type_check(code, fm_it, reg_to_check, *dex_type);
  auto type_check_insn_it = fm_it;
  // Fall through to throw Error
  fm_it = insert_throw_error(code, fm_it, callee,
                             m_config.return_value_assert_fail_handler);
  fm_it = code->insert_after(fm_it, new BranchTarget(&*null_check_insn_it));
  fm_it = code->insert_after(fm_it, new BranchTarget(&*type_check_insn_it));
  stats.return_type_check_inserted++;
  return fm_it;
}

} // namespace type_analyzer
