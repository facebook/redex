/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationRuntimeAssert.h"

#include "ProguardMap.h"
#include "Walkers.h"

namespace constant_propagation {

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

static IROpcode opcode_for_interval(const sign_domain::Interval intv) {
  using namespace sign_domain;
  switch (intv) {
  case Interval::ALL:
  case Interval::EMPTY:
    not_reached_log("Cannot generate opcode for this interval");
  case Interval::SIZE:
    not_reached_log("SIZE is not a valid interval");
  case Interval::LTZ:
    return OPCODE_IF_LTZ;
  case Interval::NEZ:
    return OPCODE_IF_NEZ;
  case Interval::GTZ:
    return OPCODE_IF_GTZ;
  case Interval::EQZ:
    return OPCODE_IF_EQZ;
  case Interval::GEZ:
    return OPCODE_IF_GEZ;
  case Interval::LEZ:
    return OPCODE_IF_LEZ;
  }
}

/*
 * Inserts a sequence of opcodes that ends in an if-* branch. The branch is
 * taken only if :reg_to_check at runtime has the value indicated by :scd.
 *
 * Returns an iterator to the if-* opcode.
 */
static IRList::iterator insert_if_opcode_check(
    IRCode* code,
    IRList::iterator it,
    reg_t reg_to_check,
    const SignedConstantDomain& scd) {
  always_assert(!scd.is_top() && !scd.is_bottom());
  const auto& cst = scd.get_constant();
  if (cst) {
    // If we have an exact constant, create a const instruction that loads
    // that value and check for equality.
    auto cst_reg = code->allocate_temp();
    it = code->insert_after(it,
                            (new IRInstruction(OPCODE_CONST))
                                ->set_dest(cst_reg)
                                ->set_literal(*cst));
    it = code->insert_after(it,
                            (new IRInstruction(OPCODE_IF_EQ))
                                ->set_src(0, reg_to_check)
                                ->set_src(1, cst_reg));
    return it;
  } else {
    // We don't have a constant, but we have a range. Insert the appropriate
    // if-* instruction that checks that the argument falls in the range.
    it = code->insert_after(
        it,
        (new IRInstruction(opcode_for_interval(scd.interval())))
            ->set_src(0, reg_to_check));
  }
  return it;
}

void RuntimeAssertTransform::apply(
    const intraprocedural::FixpointIterator& intra_cp,
    const WholeProgramState& wps,
    DexMethod* method) {
  auto* code = method->get_code();
  always_assert(code != nullptr);
  always_assert_log(!code->editable_cfg_built(),
                    "TODO: Upgrade RuntimeAssertTransform to use cfg.");
  auto ii = InstructionIterable(code);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    it = insert_field_assert(wps, code, it);
    it = insert_return_value_assert(wps, code, it);
  }
  auto env = intra_cp.get_entry_state_at(code->cfg().entry_block());
  insert_param_asserts(env, method);
}

/*
 * Insert code after each field that our static analysis thinks are constant.
 * If the runtime value differs, the code will call out to
 * field_assert_fail_handler, passing it a string that indicates the name of
 * the problematic field.
 */
ir_list::InstructionIterator RuntimeAssertTransform::insert_field_assert(
    const WholeProgramState& wps,
    IRCode* code,
    ir_list::InstructionIterator it) {
  auto* insn = it->insn;
  auto op = insn->opcode();
  if (!opcode::is_an_sget(op)) {
    return it;
  }
  auto* field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return it;
  }
  if (!(type::is_integer(field->get_type()) ||
        type::is_object(field->get_type()))) {
    return it;
  }
  auto scd = wps.get_field_value(field).maybe_get<SignedConstantDomain>();
  if (!scd || scd->is_top()) {
    return it;
  }
  auto fm_it = it.unwrap();
  auto reg = ir_list::move_result_pseudo_of(fm_it)->dest();
  ++fm_it; // skip the move-result-pseudo
  fm_it = insert_if_opcode_check(code, fm_it, reg, *scd);
  auto check_insn_it = fm_it;
  auto tmp = code->allocate_temp();
  // XXX ideally this would use the deobfuscated field name
  fm_it = code->insert_after(fm_it,
                             ((new IRInstruction(OPCODE_CONST_STRING))
                                  ->set_string(field->get_name())));
  fm_it = code->insert_after(
      fm_it,
      ((new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))->set_dest(tmp)));
  fm_it =
      code->insert_after(fm_it,
                         ((new IRInstruction(OPCODE_INVOKE_STATIC))
                              ->set_method(m_config.field_assert_fail_handler)
                              ->set_srcs_size(1)
                              ->set_src(0, tmp)));
  auto bt = new BranchTarget(&*check_insn_it);
  fm_it = code->insert_after(fm_it, bt);

  it.reset(fm_it);
  return it;
}

/*
 * Insert code after each invoke to a method which our static analysis believes
 * returns a constant value. If the runtime value differs, the code will call
 * out to return_value_assert_fail_handler, passing it a string that indicates
 * the name of the problematic method.
 *
 * For methods "returning" bottom, i.e. those that should never return, we
 * simply insert a call to the failure handler right after the invoke.
 */
ir_list::InstructionIterator RuntimeAssertTransform::insert_return_value_assert(
    const WholeProgramState& wps,
    IRCode* code,
    ir_list::InstructionIterator it) {
  auto* insn = it->insn;
  auto op = insn->opcode();
  if (!(op == OPCODE_INVOKE_DIRECT || op == OPCODE_INVOKE_STATIC)) {
    return it;
  }
  auto* callee = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (callee == nullptr) {
    return it;
  }
  auto insert_assertion = [this, callee, code](IRList::iterator fm_it) {
    auto tmp = code->allocate_temp();
    fm_it = code->insert_after(fm_it,
                               ((new IRInstruction(OPCODE_CONST_STRING))
                                    ->set_string(callee->get_name())));
    fm_it = code->insert_after(
        fm_it,
        ((new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
             ->set_dest(tmp)));
    fm_it = code->insert_after(
        fm_it,
        ((new IRInstruction(OPCODE_INVOKE_STATIC))
             ->set_method(m_config.return_value_assert_fail_handler)
             ->set_srcs_size(1)
             ->set_src(0, tmp)));
    return fm_it;
  };
  auto cst = wps.get_return_value(callee);
  if (cst.is_bottom()) {
    if (opcode::is_a_move_result(std::next(it)->insn->opcode())) {
      ++it;
    }
    it.reset(insert_assertion(it.unwrap()));
    return it;
  }

  ++it;
  if (!opcode::is_a_move_result(it->insn->opcode())) {
    return it;
  }
  auto reg = it->insn->dest();
  auto ret_type = callee->get_proto()->get_rtype();
  if (!(type::is_integer(ret_type) || type::is_object(ret_type))) {
    return it;
  }
  auto scd = cst.maybe_get<SignedConstantDomain>();
  if (!scd || scd->is_top()) {
    return it;
  }
  auto fm_it = it.unwrap();
  fm_it = insert_if_opcode_check(code, fm_it, reg, *scd);
  auto check_insn_it = fm_it;
  fm_it = insert_assertion(fm_it);
  auto bt = new BranchTarget(&*check_insn_it);
  fm_it = code->insert_after(fm_it, bt);

  it.reset(fm_it);
  return it;
}

/*
 * Insert code at the start of the method that checks that the arguments that
 * our static analysis thinks are constant actually have those values at
 * runtime. If the check fails, the code will call out to
 * param_assert_fail_handler, passing it a single integer that indicates the
 * index of the failing parameter.
 */
void RuntimeAssertTransform::insert_param_asserts(
    const ConstantEnvironment& env, DexMethod* method) {
  const auto& args = env.get_register_environment();
  if (!args.is_value()) {
    return;
  }
  auto& code = *method->get_code();
  const auto* arg_types = method->get_proto()->get_args();
  auto param_insns = code.get_param_instructions();
  auto insert_it = param_insns.end();
  auto insn_it = ir_list::InstructionIterable(code).begin();
  if (!is_static(method)) {
    // Skip the load-param instruction for the `this` argument
    ++insn_it;
  }
  // We do not want to iterate over InstructionIterable(param_insns) here
  // because we are inserting MIEs that will move the end iterator of
  // param_insns
  for (uint32_t i = 0; i < arg_types->size(); ++i, ++insn_it) {
    auto* arg_type = arg_types->at(i);
    // We don't currently support floating-point or long types...
    if (!(type::is_integer(arg_type) || type::is_object(arg_type))) {
      continue;
    }
    auto reg = insn_it->insn->dest();
    auto scd_opt = args.get(reg).maybe_get<SignedConstantDomain>();
    if (!scd_opt) {
      continue;
    }
    auto& scd = *scd_opt;
    // The branching instruction that checks whether the constant domain is
    // correct for the given param
    // XXX with some refactoring, we could use insert_if_opcode_check here...
    IRList::iterator check_insn_it;
    const auto& cst = scd.get_constant();
    if (cst) {
      // If we have an exact constant, create a const instruction that loads
      // that value and check for equality.
      auto cst_reg = code.allocate_temp();
      code.insert_before(insert_it,
                         (new IRInstruction(OPCODE_CONST))
                             ->set_dest(cst_reg)
                             ->set_literal(*cst));
      check_insn_it = code.insert_before(insert_it,
                                         (new IRInstruction(OPCODE_IF_EQ))
                                             ->set_src(0, reg)
                                             ->set_src(1, cst_reg));
    } else {
      // We don't have a constant, but we have a range. Insert the appropriate
      // if-* instruction that checks that the argument falls in the range.
      check_insn_it = code.insert_before(
          insert_it,
          (new IRInstruction(opcode_for_interval(scd.interval())))
              ->set_src(0, reg));
    }
    // If the branch in check_insn_it does not get taken, it means the
    // check failed. So we call the error handler here.
    auto tmp = code.allocate_temp();
    code.insert_before(
        insert_it,
        ((new IRInstruction(OPCODE_CONST))->set_dest(tmp)->set_literal(i)));
    code.insert_before(insert_it,
                       ((new IRInstruction(OPCODE_INVOKE_STATIC))
                            ->set_method(m_config.param_assert_fail_handler)
                            ->set_srcs_size(1)
                            ->set_src(0, tmp)));
    auto bt = new BranchTarget(&*check_insn_it);
    code.insert_before(insert_it, bt);
  }
}

} // namespace constant_propagation
