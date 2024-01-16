/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationRuntimeAssert.h"

#include "ControlFlow.h"
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

/* Insert an asssertion after \p it. This is only used in return value check.*/
static void insert_assertion(cfg::ControlFlowGraph& cfg,
                             cfg::InstructionIterator& it,
                             const DexMethod* member,
                             DexMethodRef* handler) {
  auto member_name_reg = cfg.allocate_temp();
  IRInstruction* const_insn =
      (new IRInstruction(OPCODE_CONST_STRING))->set_string(member->get_name());
  IRInstruction* move_insn =
      (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
          ->set_dest(member_name_reg);
  IRInstruction* invoke_insn = (new IRInstruction(OPCODE_INVOKE_STATIC))
                                   ->set_method(handler)
                                   ->set_srcs_size(1)
                                   ->set_src(0, member_name_reg);
  cfg.insert_after(it, {const_insn, move_insn, invoke_insn});
}

/*
 *First inserts a sequence of opcodes that ends in an if-* branch. The branch is
 * taken only if \p reg_to_check at runtime has the value indicated by \p scd.
 * Then add a new block to handle assertion. \p reg_to_check could come from a
 * return value, a field or a param. If \p member is not null, it means \p
 * reg_to_check is from either a field or a return value. Otherwise, it is from
 * the \param_idx -th params.
 */
template <typename DexMember>
static void insert_if_check_with_assertion(cfg::ControlFlowGraph& cfg,
                                           cfg::InstructionIterator& it,
                                           reg_t reg_to_check,
                                           const SignedConstantDomain& scd,
                                           const DexMember* member,
                                           const uint32_t param_idx,
                                           bool is_param_idx,
                                           DexMethodRef* handler) {
  always_assert(!scd.is_top() && !scd.is_bottom());
  const auto& cst = scd.get_constant();
  // 1. Split it into B1->B2, and current 'it' is the last insn of B1.
  cfg::Block* B1 = it.block();
  cfg::Block* B2 = cfg.split_block(it);
  cfg.delete_edges_between(B1, B2);
  // 2. Create a new block throw_block for throwing error.
  cfg::Block* throw_block = cfg.create_block();
  auto member_name_reg = cfg.allocate_temp();
  if (!is_param_idx) {
    IRInstruction* const_insn = (new IRInstruction(OPCODE_CONST_STRING))
                                    ->set_string(member->get_name());
    IRInstruction* move_insn =
        (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
            ->set_dest(member_name_reg);
    IRInstruction* invoke_insn = (new IRInstruction(OPCODE_INVOKE_STATIC))
                                     ->set_method(handler)
                                     ->set_srcs_size(1)
                                     ->set_src(0, member_name_reg);
    throw_block->push_back({const_insn, move_insn, invoke_insn});
  } else {
    IRInstruction* const_insn = (new IRInstruction(OPCODE_CONST))
                                    ->set_dest(member_name_reg)
                                    ->set_literal(param_idx);
    IRInstruction* invoke_insn = (new IRInstruction(OPCODE_INVOKE_STATIC))
                                     ->set_method(handler)
                                     ->set_srcs_size(1)
                                     ->set_src(0, member_name_reg);
    throw_block->push_back({const_insn, invoke_insn});
  }
  // 3. Insert a null-check at the end of B1, and create edges between B1, B2,
  // and throw_block.
  if (cst) {
    // If we have an exact constant, create a const instruction that loads
    // that value and check for equality.
    auto cst_reg = cfg.allocate_temp();
    cfg.insert_after(it,
                     (new IRInstruction(OPCODE_CONST))
                         ->set_dest(cst_reg)
                         ->set_literal(*cst));
    IRInstruction* if_insn = (new IRInstruction(OPCODE_IF_EQ))
                                 ->set_src(0, reg_to_check)
                                 ->set_src(1, cst_reg);
    cfg.create_branch(B1, if_insn, throw_block, B2);
    cfg.add_edge(throw_block, B2, cfg::EDGE_GOTO);
  } else {
    // We don't have a constant, but we have a range. Insert the appropriate
    // if-* instruction that checks that the argument falls in the range.
    IRInstruction* if_insn =
        (new IRInstruction(opcode_for_interval(scd.interval())))
            ->set_src(0, reg_to_check);
    cfg.create_branch(B1, if_insn, throw_block, B2);
    cfg.add_edge(throw_block, B2, cfg::EDGE_GOTO);
  }
}

void RuntimeAssertTransform::apply(
    const intraprocedural::FixpointIterator& intra_cp,
    const WholeProgramState& wps,
    DexMethod* method) {
  auto* code = method->get_code();
  always_assert(code != nullptr);
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  auto ii = cfg::InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    // We may insert insns/branches after current it.  Therefore, we record the
    // next iterator position, to skip any new insns/branches in the next
    // iteration.
    auto next_it = std::next(it);
    bool changed;
    changed = insert_field_assert(wps, cfg, it);
    if (changed) {
      // Some code has been inserted. Skip the those code.
      it = next_it;
    }
    changed = insert_return_value_assert(wps, cfg, it);
    if (changed) {
      // Some code has been inserted. Skip the those code.
      it = next_it;
    }
  }
  auto env = intra_cp.get_entry_state_at(cfg.entry_block());
  insert_param_asserts(env, method);
}

/*
 * Insert code after each field that our static analysis thinks are constant.
 * If the runtime value differs, the code will call out to
 * field_assert_fail_handler, passing it a string that indicates the name of
 * the problematic field.
 */
bool RuntimeAssertTransform::insert_field_assert(const WholeProgramState& wps,
                                                 cfg::ControlFlowGraph& cfg,
                                                 cfg::InstructionIterator& it) {
  auto* insn = it->insn;
  auto op = insn->opcode();
  if (!opcode::is_an_sget(op)) {
    return false;
  }
  auto* field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return false;
  }
  if (!(type::is_integral(field->get_type()) ||
        type::is_object(field->get_type()))) {
    return false;
  }
  auto scd = wps.get_field_value(field).maybe_get<SignedConstantDomain>();
  if (!scd || scd->is_top()) {
    return false;
  }

  if (!insn->has_move_result_pseudo()) {
    return false;
  }
  auto mov_res_it = cfg.move_result_of(it);
  if (mov_res_it.is_end()) {
    return false;
  }

  auto reg_to_check = mov_res_it->insn->dest();
  insert_if_check_with_assertion(cfg, mov_res_it, reg_to_check, *scd, field, 0,
                                 false, m_config.field_assert_fail_handler);
  return true;
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
bool RuntimeAssertTransform::insert_return_value_assert(
    const WholeProgramState& wps,
    cfg::ControlFlowGraph& cfg,
    cfg::InstructionIterator& it) {
  auto* insn = it->insn;
  auto op = insn->opcode();
  if (!(op == OPCODE_INVOKE_DIRECT || op == OPCODE_INVOKE_STATIC)) {
    return false;
  }
  auto* callee = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (callee == nullptr) {
    return false;
  }

  auto cst = wps.get_return_value(callee);
  auto mov_res_it = cfg.move_result_of(it);

  if (cst.is_bottom()) {
    if (!mov_res_it.is_end()) {
      insert_assertion(cfg, mov_res_it, callee,
                       m_config.return_value_assert_fail_handler);
    } else {
      insert_assertion(cfg, it, callee,
                       m_config.return_value_assert_fail_handler);
    }
    return true;
  }

  if (mov_res_it.is_end()) {
    return false;
  }

  auto ret_type = callee->get_proto()->get_rtype();
  if (!(type::is_integral(ret_type) || type::is_object(ret_type))) {
    return false;
  }
  auto scd = cst.maybe_get<SignedConstantDomain>();
  if (!scd || scd->is_top()) {
    return false;
  }

  auto reg_to_check = mov_res_it->insn->dest();
  insert_if_check_with_assertion(cfg, mov_res_it, reg_to_check, *scd, callee, 0,
                                 false,
                                 m_config.return_value_assert_fail_handler);
  return true;
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
  auto code = method->get_code();
  auto& cfg = code->cfg();
  const auto* arg_types = method->get_proto()->get_args();
  auto param_insns = cfg.get_param_instructions();
  auto insert_it = cfg.entry_block()->get_last_param_loading_insn();
  auto cfg_insert_it =
      cfg.entry_block()->to_cfg_instruction_iterator(insert_it);

  // We do not want to iterate over InstructionIterable(param_insns) here
  // because we are inserting MIEs that will move the end iterator of
  // param_insns
  uint32_t i = 0;
  auto ii = InstructionIterable(param_insns);
  auto arg_size = arg_types->size();
  if (!is_static(method)) {
    arg_size++;
  }
  for (auto it = ii.begin(); i < arg_size; ++it, ++i) {
    if (!is_static(method) && i == 0) {
      // Skip the load-param instruction for the `this` argument
      continue;
    }
    auto idx = is_static(method) ? i : i - 1;
    auto* arg_type = arg_types->at(idx);
    // We don't currently support floating-point or long types...
    if (!(type::is_integral(arg_type) || type::is_object(arg_type))) {
      continue;
    }
    auto cfg_it = cfg.entry_block()->to_cfg_instruction_iterator(it);
    auto reg_to_check = cfg_it->insn->dest();
    auto scd_opt = args.get(reg_to_check).maybe_get<SignedConstantDomain>();
    if (!scd_opt) {
      continue;
    }
    auto& scd = *scd_opt;
    insert_if_check_with_assertion(cfg, cfg_insert_it, reg_to_check, scd,
                                   method, idx, true,
                                   m_config.param_assert_fail_handler);
  }
}
} // namespace constant_propagation
