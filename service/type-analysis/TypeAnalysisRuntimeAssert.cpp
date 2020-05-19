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
                               ->set_string(member->get_name())));
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

RuntimeAssertTransform::Stats RuntimeAssertTransform::apply(
    const local::LocalTypeAnalyzer& /* lta */,
    const WholeProgramState& wps,
    DexMethod* method) {
  auto* code = method->get_code();
  always_assert(code != nullptr);
  RuntimeAssertTransform::Stats stats{};
  auto ii = InstructionIterable(code);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    it = insert_field_assert(wps, code, it, stats);
    it = insert_return_value_assert(wps, code, it, stats);
  }
  return stats;
}

ir_list::InstructionIterator RuntimeAssertTransform::insert_field_assert(
    const WholeProgramState& wps,
    IRCode* code,
    ir_list::InstructionIterator it,
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
  auto fm_it = it.unwrap();
  auto reg_to_check = ir_list::move_result_pseudo_of(fm_it)->dest();
  ++fm_it; // skip the move-result-pseudo
  fm_it = insert_type_check(code, fm_it, reg_to_check, *dex_type);
  auto check_insn_it = fm_it;
  // Fall through to throw Error
  fm_it = insert_throw_error(code, fm_it, field,
                             m_config.field_assert_fail_handler);
  auto branch_target = new BranchTarget(&*check_insn_it);
  fm_it = code->insert_after(fm_it, branch_target);
  stats.field_type_check_inserted++;
  it.reset(fm_it);
  return it;
}

ir_list::InstructionIterator RuntimeAssertTransform::insert_return_value_assert(
    const WholeProgramState& wps,
    IRCode* code,
    ir_list::InstructionIterator it,
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

  ++it;
  if (!opcode::is_move_result(it->insn->opcode())) {
    return it;
  }
  auto reg_to_check = it->insn->dest();
  auto fm_it = it.unwrap();
  fm_it = insert_type_check(code, fm_it, reg_to_check, *dex_type);
  auto check_insn_it = fm_it;
  // Fall through to throw Error
  fm_it = insert_throw_error(code, fm_it, callee,
                             m_config.return_value_assert_fail_handler);
  auto branch_target = new BranchTarget(&*check_insn_it);
  fm_it = code->insert_after(fm_it, branch_target);
  stats.return_type_check_inserted++;

  it.reset(fm_it);
  return it;
}

} // namespace type_analyzer
