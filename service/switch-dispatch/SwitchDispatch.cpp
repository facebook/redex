/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "SwitchDispatch.h"

#include "Creators.h"
#include "TypeReference.h"

using namespace type_reference;

namespace {

/**
 * This is a soft limit used to detect a large dispatch.
 * Further decision is made based on the total instruction count of the
 * dispatch.
 */
constexpr uint64_t MAX_NUM_DISPATCH_TARGET = 500;

/**
 * Some versions of ART (5.0.0 - 5.0.2) will fail to verify a method if it
 * is too large. See https://code.google.com/p/android/issues/detail?id=66655.
 *
 * Although the limit is only applicable to dex2oat dependent build, we want to
 * avoid that from happening wherever Type Erasure is enabled. Since we want to
 * leave some room for accommodating the injected switch dispatch code, the
 * number here is lower than the actual limit.
 */
constexpr uint64_t MAX_NUM_DISPATCH_INSTRUCTION = 40000;

MethodCreator* init_method_creator(const dispatch::Spec& spec,
                                   DexMethod* orig_method) {
  return new MethodCreator(spec.owner_type,
                           DexString::make_string(spec.name),
                           spec.proto,
                           spec.access_flags,
                           orig_method->get_anno_set());
}

void emit_call(const dispatch::Spec& spec,
               IROpcode opcode,
               const std::vector<Location>& args,
               Location& res_loc,
               DexMethod* callee,
               MethodBlock* block) {
  block->invoke(opcode, callee, args);
  if (!spec.proto->is_void()) {
    block->move_result(res_loc, spec.proto->get_rtype());
  }
}

void invoke_static(const dispatch::Spec& spec,
                   const std::vector<Location>& args,
                   Location& res_loc,
                   DexMethod* callee,
                   MethodBlock* block) {
  emit_call(spec, OPCODE_INVOKE_STATIC, args, res_loc, callee, block);
}

void emit_check_cast(const dispatch::Spec& spec,
                     std::vector<Location>& args,
                     DexMethod* callee,
                     MethodBlock* block) {
  if (args.size() && spec.proto->get_args()->size()) {
    auto dispatch_head_arg_type =
        spec.proto->get_args()->get_type_list().front();
    auto callee_head_arg_type =
        callee->get_proto()->get_args()->get_type_list().front();
    if (dispatch_head_arg_type != callee_head_arg_type) {
      block->check_cast(args.front(), callee_head_arg_type);
    }
  }
}

/**
 * To simplify control flows, if the spec proto is void, we simply return a
 * dummy Location here. In this case, the subsequent return instruciton will be
 * a no-op one.
 */
Location get_return_location(const dispatch::Spec& spec, MethodCreator* mc) {
  return spec.proto->is_void() ? mc->get_local(0) // not used
                               : mc->make_local(spec.proto->get_rtype());
}

bool save_type_tag_to_field(const dispatch::Spec& spec) {
  return spec.type == dispatch::Type::CTOR_WITH_TYPE_TAG_PARAM;
}

bool is_ctor(const dispatch::Spec& spec) {
  return spec.type == dispatch::Type::CTOR_WITH_TYPE_TAG_PARAM ||
         spec.type == dispatch::Type::CTOR;
}

/**
 * In case the method overrides one of the super classes implementation, default
 * to that implementation.
 */
void handle_default_block(
    const dispatch::Spec& spec,
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee,
    const std::vector<Location>& args,
    Location& ret_loc,
    MethodBlock* def_block) {
  if (is_ctor(spec)) {
    // Handle the last case in default block.
    auto last_indices = indices_to_callee.rbegin()->first;
    auto last_callee = indices_to_callee.at(last_indices);
    invoke_static(spec, args, ret_loc, last_callee, def_block);
    return;
  }
  if (spec.overridden_meth) {
    always_assert_log(spec.overridden_meth->is_virtual(),
                      "non-virtual overridden method %s\n",
                      SHOW(spec.overridden_meth));
    emit_call(spec, OPCODE_INVOKE_SUPER, args, ret_loc, spec.overridden_meth,
              def_block);
  } else if (!spec.proto->is_void()) {
    def_block->init_loc(ret_loc);
  }
}

// If there is no need for the switch statement.
bool is_single_target_case(
    const dispatch::Spec& spec,
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee) {
  return indices_to_callee.size() == 1 && !spec.overridden_meth;
}

std::map<SwitchIndices, MethodBlock*> get_switch_cases(
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee,
    const bool skip_last_case = false) {
  auto last_indices = indices_to_callee.rbegin()->first;
  std::map<SwitchIndices, MethodBlock*> cases;
  for (auto& it : indices_to_callee) {
    if (skip_last_case && it.first == last_indices) {
      continue;
    }
    cases[it.first] = nullptr;
  }
  return cases;
}

DexMethod* materialize_dispatch(DexMethod* orig_method, MethodCreator* mc) {
  auto dispatch = mc->create();
  dispatch->rstate = orig_method->rstate;
  set_public(dispatch);
  TRACE(SDIS,
        9,
        " created dispatch: %s\n%s\n",
        SHOW(dispatch),
        SHOW(dispatch->get_code()));

  return dispatch;
}

/**
 * Given all the method targets have the same proto, args will be the same
 * between them.
 */
std::vector<Location> get_args_from(DexMethod* method, MethodCreator* mc) {
  std::vector<Location> args;
  size_t args_size = method->get_proto()->get_args()->size();
  for (size_t arg_loc = 0; arg_loc < args_size; ++arg_loc) {
    args.push_back(mc->get_local(arg_loc));
  }

  return args;
}

size_t estimate_num_switch_dispatch_needed(
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee,
    const boost::optional<size_t> max_num_dispatch_target = boost::none) {
  // Analyze the size of the dispatch
  size_t num_cases = indices_to_callee.size();
  // If the config is enabled we shortcut the instruction count limit.
  // This should only happen for testing.
  TRACE(SDIS, 9, "num cases %d, max num dispatch targets %d\n", num_cases,
        max_num_dispatch_target.get_value_or(0));
  if (max_num_dispatch_target != boost::none &&
      num_cases > max_num_dispatch_target.get()) {
    return ceil(static_cast<float>(num_cases) / max_num_dispatch_target.get());
  }
  if (num_cases > MAX_NUM_DISPATCH_TARGET) {
    size_t total_num_insn = 0;
    for (auto& it : indices_to_callee) {
      auto target = it.second;
      total_num_insn += target->get_code()->count_opcodes();
    }

    return ceil(static_cast<float>(total_num_insn) /
                MAX_NUM_DISPATCH_INSTRUCTION);
  }

  return 1;
}

/**
 * Create a simple single level switch based dispatch method.
 * We here construct a leaf level dispatch assuming all targets are dedupped.
 */
DexMethod* create_simple_switch_dispatch(
    const dispatch::Spec& spec,
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee) {
  always_assert(indices_to_callee.size());
  TRACE(SDIS,
        5,
        "creating leaf switch dispatch %s.%s for targets of size %d\n",
        SHOW(spec.owner_type),
        spec.name.c_str(),
        indices_to_callee.size());
  auto orig_method = indices_to_callee.begin()->second;
  auto mc = init_method_creator(spec, orig_method);
  auto self_loc = mc->get_local(0);
  // iget type tag field.
  auto type_tag_loc = mc->make_local(get_int_type());
  auto ret_loc = get_return_location(spec, mc);
  auto mb = mc->get_main_block();

  std::vector<Location> args = get_args_from(orig_method, mc);

  // Extra checks if there is no need for the switch statement.
  if (is_single_target_case(spec, indices_to_callee)) {
    invoke_static(spec, args, ret_loc, orig_method, mb);
    mb->ret(spec.proto->get_rtype(), ret_loc);
    return materialize_dispatch(orig_method, mc);
  }

  mb->iget(spec.type_tag_field, self_loc, type_tag_loc);
  auto cases = get_switch_cases(indices_to_callee);

  // default case and return
  auto def_block = mb->switch_op(type_tag_loc, cases);
  handle_default_block(spec, indices_to_callee, args, ret_loc, def_block);
  mb->ret(spec.proto->get_rtype(), ret_loc);

  for (auto& case_it : cases) {
    auto case_block = case_it.second;
    always_assert(case_block != nullptr);
    auto callee = indices_to_callee.at(case_it.first);
    always_assert(is_static(callee));
    // check-cast and call
    emit_check_cast(spec, args, callee, case_block);
    invoke_static(spec, args, ret_loc, callee, case_block);
  }

  return materialize_dispatch(orig_method, mc);
}

dispatch::DispatchMethod create_two_level_switch_dispatch(
    const size_t num_switch_needed,
    const dispatch::Spec& spec,
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee) {
  auto orig_method = indices_to_callee.begin()->second;
  auto mc = init_method_creator(spec, orig_method);
  auto self_loc = mc->get_local(0);
  // iget type tag field.
  auto type_tag_loc = mc->make_local(get_int_type());
  auto mb = mc->get_main_block();
  auto ret_loc = get_return_location(spec, mc);

  std::vector<Location> args = get_args_from(orig_method, mc);

  mb->iget(spec.type_tag_field, self_loc, type_tag_loc);
  auto cases = get_switch_cases(indices_to_callee);

  // default case and return
  auto def_block = mb->switch_op(type_tag_loc, cases);
  handle_default_block(spec, indices_to_callee, args, ret_loc, def_block);
  mb->ret(spec.proto->get_rtype(), ret_loc);

  size_t max_num_leaf_switch = cases.size() / num_switch_needed + 1;
  std::map<SwitchIndices, DexMethod*> sub_indices_to_callee;
  std::vector<DexMethod*> sub_dispatches;
  size_t dispatch_index = 0;
  size_t case_index = 0;
  size_t subcase_count = 0;
  for (auto& case_it : cases) {
    sub_indices_to_callee[case_it.first] = indices_to_callee.at(case_it.first);
    subcase_count++;

    if (subcase_count == max_num_leaf_switch ||
        case_index == cases.size() - 1) {
      auto sub_name = spec.name + "$" + std::to_string(dispatch_index);
      auto new_arg_list =
          prepend_and_make(spec.proto->get_args(), spec.owner_type);
      auto static_dispatch_proto =
          DexProto::make_proto(spec.proto->get_rtype(), new_arg_list);
      dispatch::Spec sub_spec{
          spec.owner_type,
          dispatch::Type::VIRTUAL,
          sub_name,
          static_dispatch_proto,
          spec.access_flags | ACC_STATIC,
          spec.type_tag_field,
          nullptr // overridden_method
      };
      auto sub_dispatch =
          create_simple_switch_dispatch(sub_spec, sub_indices_to_callee);
      sub_indices_to_callee.clear();

      auto case_block = case_it.second;
      always_assert(case_block != nullptr);
      // check-cast and call
      emit_check_cast(spec, args, sub_dispatch, case_block);
      invoke_static(spec, args, ret_loc, sub_dispatch, case_block);

      sub_dispatches.push_back(sub_dispatch);
      dispatch_index++;
      subcase_count = 0;
    }

    case_index++;
  }

  auto dispatch_meth = materialize_dispatch(orig_method, mc);

  /////////////////////////////////////////////////////////////////////////////
  // Remove unwanted gotos.
  //
  // Creator produces a complete switch statement with all cases ending with a
  // goto to the end of the switch statement. This is not the intended control
  // flow here. We need to remove the unwanted gotos to enable fall-throughs to
  // the case that we call the second level dispatch.
  //
  // An example dispatch before removing unwanted gotos:
  // [0x3a7f260] OPCODE: IOPCODE_LOAD_PARAM_OBJECT v2
  // [0x3a7eb60] OPCODE: IGET v2, LDummyShape;.$t:I
  // [0x3a64c60] OPCODE: IOPCODE_MOVE_RESULT_PSEUDO v0
  // [0x37c5300] OPCODE: PACKED_SWITCH v0
  // [0x37e0590] OPCODE: INVOKE_SUPER v2, LBase;.doStuff:()I
  // [0x3a71590] OPCODE: MOVE_RESULT v1
  // [0x3a18460] TARGET: SIMPLE 0x37c3f10
  // [0x3a2a4a0] TARGET: SIMPLE 0x3a66c90
  // [0x3a6ac90] TARGET: SIMPLE 0x3a4a310
  // [0x3a32920] TARGET: SIMPLE 0x3918fe0
  // [0x348d3a0] OPCODE: RETURN v1
  // [0x3806200] TARGET: MULTI 0 0x37c5300
  // [0x37c3f10] OPCODE: GOTO
  // [0x37dd7b0] TARGET: MULTI 1 0x37c5300
  // [0x3a4bd30] OPCODE: INVOKE_STATIC v2, dispatch$0:(LDummyShape;)I
  // [0x37a8c90] OPCODE: MOVE_RESULT v1
  // [0x3a66c90] OPCODE: GOTO
  // [0x37bda90] TARGET: MULTI 2 0x37c5300
  // [0x3a4a310] OPCODE: GOTO
  // [0x3a65ef0] TARGET: MULTI 3 0x37c5300
  // [0x3815180] OPCODE: INVOKE_STATIC v2, dispatch$1:(LDummyShape;)I
  // [0x37c84d0] OPCODE: MOVE_RESULT v1
  // [0x3918fe0] OPCODE: GOTO
  //
  // After removing gotos:
  // [0x3a7f260] OPCODE: IOPCODE_LOAD_PARAM_OBJECT v2
  // [0x3a7eb60] OPCODE: IGET v2, LDummyShape;.$t:I
  // [0x3a64c60] OPCODE: IOPCODE_MOVE_RESULT_PSEUDO v0
  // [0x37c5300] OPCODE: PACKED_SWITCH v0
  // [0x37e0590] OPCODE: INVOKE_SUPER v2, LBase;.doStuff:()I
  // [0x3a71590] OPCODE: MOVE_RESULT v1
  // [0x3a2a4a0] TARGET: SIMPLE 0x3a66c90
  // [0x3a32920] TARGET: SIMPLE 0x3918fe0
  // [0x348d3a0] OPCODE: RETURN v1
  // [0x3806200] TARGET: MULTI 0 0x37c5300
  // [0x37dd7b0] TARGET: MULTI 1 0x37c5300
  // [0x3a4bd30] OPCODE: INVOKE_STATIC v2, dispatch$0:(LDummyShape;)I
  // [0x37a8c90] OPCODE: MOVE_RESULT v1
  // [0x3a66c90] OPCODE: GOTO
  // [0x37bda90] TARGET: MULTI 2 0x37c5300
  // [0x3a65ef0] TARGET: MULTI 3 0x37c5300
  // [0x3815180] OPCODE: INVOKE_STATIC v2, dispatch$1:(LDummyShape;)I
  // [0x37c84d0] OPCODE: MOVE_RESULT v1
  // [0x3918fe0] OPCODE: GOTO
  /////////////////////////////////////////////////////////////////////////////
  std::vector<IRList::iterator> to_delete;
  auto code = dispatch_meth->get_code();
  for (auto it = code->begin(); it != code->end(); ++it) {
    if (it->type == MFLOW_OPCODE && is_goto(it->insn->opcode()) &&
        std::prev(it)->type == MFLOW_TARGET) {
      to_delete.emplace_back(it);
    }
  }

  for (const auto& it : to_delete) {
    code->remove_opcode(it);
  }

  TRACE(SDIS, 9, "dispatch: split dispatch %s\n%s\n", SHOW(dispatch_meth),
        SHOW(dispatch_meth->get_code()));
  dispatch::DispatchMethod dispatch_method{dispatch_meth, sub_dispatches};
  return dispatch_method;
}

} // namespace

namespace dispatch {

DispatchMethod create_virtual_dispatch(
    const Spec& spec,
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee) {

  size_t num_switch_needed = estimate_num_switch_dispatch_needed(
      indices_to_callee, spec.max_num_dispatch_target);
  if (num_switch_needed == 1) {
    auto main_dispatch = create_simple_switch_dispatch(spec, indices_to_callee);
    dispatch::DispatchMethod dispatch{main_dispatch};
    return dispatch;
  }

  TRACE(SDIS, 5, "splitting large dispatch %s.%s into %d\n",
        SHOW(spec.owner_type), spec.name.c_str(), num_switch_needed);
  return create_two_level_switch_dispatch(num_switch_needed, spec,
                                          indices_to_callee);
}

DexMethod* create_ctor_or_static_dispatch(
    const Spec& spec,
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee) {
  always_assert(indices_to_callee.size() && spec.overridden_meth == nullptr);
  TRACE(SDIS,
        5,
        "creating dispatch %s.%s for targets of size %d\n",
        SHOW(spec.owner_type),
        spec.name.c_str(),
        indices_to_callee.size());
  auto orig_method = indices_to_callee.begin()->second;
  auto mc = init_method_creator(spec, orig_method);
  auto dispatch_arg_list = spec.proto->get_args();
  auto type_tag_loc =
      mc->get_local(is_static(spec.access_flags) ? dispatch_arg_list->size() - 1
                                                 : dispatch_arg_list->size());
  auto ret_loc = get_return_location(spec, mc);
  auto mb = mc->get_main_block();
  // Set type tag field only when using synthesized type tags.
  // For the external type tag case (GQL), merged ctors take care of that
  // automatically.
  if (save_type_tag_to_field(spec)) {
    mb->iput(spec.type_tag_field, ret_loc, type_tag_loc);
  }

  // Setup switch cases
  // The MethodBlocks are to be initialized by switch_op() based on their
  // corresponding keys in the map.
  std::vector<Location> args = get_args_from(orig_method, mc);
  if (is_single_target_case(spec, indices_to_callee)) {
    invoke_static(spec, args, ret_loc, orig_method, mb);
    mb->ret(spec.proto->get_rtype(), ret_loc);
    return materialize_dispatch(orig_method, mc);
  }

  auto cases = get_switch_cases(indices_to_callee, is_ctor(spec));

  // TODO (zwei): better dispatch? E.g., push down invoke-direct to the super
  // ctor to happen after the switch stmt.
  auto def_block = mb->switch_op(type_tag_loc, cases);
  handle_default_block(spec, indices_to_callee, args, ret_loc, def_block);
  mb->ret(spec.proto->get_rtype(), ret_loc);

  for (auto& case_it : cases) {
    auto case_block = case_it.second;
    always_assert(case_block != nullptr);
    auto callee = indices_to_callee.at(case_it.first);
    always_assert(is_static(callee));
    invoke_static(spec, args, ret_loc, callee, case_block);
  }

  return materialize_dispatch(orig_method, mc);
}

} // namespace dispatch
