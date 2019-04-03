/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
                           orig_method->get_anno_set(),
                           spec.keep_debug_info);
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
  return spec.type == dispatch::Type::CTOR_SAVE_TYPE_TAG_PARAM;
}

bool is_ctor(const dispatch::Spec& spec) {
  return spec.type == dispatch::Type::CTOR_SAVE_TYPE_TAG_PARAM ||
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
    MethodCreator* mc,
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
    // dex2oat doesn't verify the simple init if the return type is an array
    // type.
    if (is_array(spec.proto->get_rtype())) {
      Location size_loc = mc->make_local(get_int_type());
      def_block->init_loc(size_loc);
      def_block->new_array(spec.proto->get_rtype(), size_loc, ret_loc);
    } else {
      def_block->init_loc(ret_loc);
    }
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
  handle_default_block(spec, indices_to_callee, args, mc, ret_loc, def_block);
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
  handle_default_block(spec, indices_to_callee, args, mc, ret_loc, def_block);
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
      dispatch::Spec sub_spec{spec.owner_type,
                              dispatch::Type::VIRTUAL,
                              sub_name,
                              static_dispatch_proto,
                              spec.access_flags | ACC_STATIC,
                              spec.type_tag_field,
                              nullptr, // overridden_method,
                              spec.keep_debug_info};
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

/**
 * This is an informal classification since we only care about direct, static,
 * virtual and constructor methods when creating dispatch method.
 */
dispatch::Type possible_type(DexMethod* method) {
  if (method->is_external() || !method->get_code()) {
    return dispatch::OTHER_TYPE;
  }
  if (is_init(method)) {
    return dispatch::CTOR;
  } else if (is_static(method)) {
    return dispatch::STATIC;
  } else if (method->is_virtual()) {
    return dispatch::VIRTUAL;
  } else {
    return dispatch::DIRECT;
  }
}

DexProto* append_int_arg(DexProto* proto) {
  auto args_list = proto->get_args()->get_type_list();
  args_list.push_back(get_int_type());
  return DexProto::make_proto(
      proto->get_rtype(), DexTypeList::make_type_list(std::move(args_list)));
}

#define LOG_AND_RETURN(fmt, ...)                       \
  do {                                                 \
    fprintf(stderr, "[dispatch] " fmt, ##__VA_ARGS__); \
    return nullptr;                                    \
  } while (0)
/**
 * Check that all the methods have the the same proto and all of them should be
 * direct, static, or virtual, create a method ref with an additional method tag
 * argument.
 */
DexMethodRef* create_dispatch_method_ref(
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee) {

  if (indices_to_callee.size() < 2) {
    LOG_AND_RETURN("Not enough methods(should >= 2) in indices_to_callee %lu\n",
                   indices_to_callee.size());
  }
  auto first_method = indices_to_callee.begin()->second;
  auto this_type =
      is_static(first_method) ? nullptr : first_method->get_class();
  auto method_type = possible_type(first_method);
  if (method_type != dispatch::STATIC && method_type != dispatch::VIRTUAL &&
      method_type != dispatch::DIRECT) {
    LOG_AND_RETURN("Unsuported method type %u(%x) for %s\n", method_type,
                   first_method->get_access(), SHOW(first_method));
  }

  for (auto& p : indices_to_callee) {
    auto meth = p.second;
    auto cur_meth_type = possible_type(meth);
    if (cur_meth_type != method_type) {
      LOG_AND_RETURN(
          "Different method type: %u(%x) for %s v.s. %u(%x) for %s\n",
          method_type, first_method->get_access(), SHOW(first_method),
          cur_meth_type, meth->get_access(), SHOW(meth));
    }
    if (this_type && this_type != meth->get_class()) {
      LOG_AND_RETURN("Different `this` type : %s v.s. %s\n", SHOW(first_method),
                     SHOW(meth));
    }
    if (meth->get_proto() != first_method->get_proto()) {
      LOG_AND_RETURN("Different protos : %s v.s. %s\n", SHOW(first_method),
                     SHOW(meth));
    }
  }
  auto cls = first_method->get_class();
  auto dispatch_proto = append_int_arg(first_method->get_proto());
  auto dispatch_name =
      dispatch::gen_dispatch_name(cls, dispatch_proto, first_method->str());
  return DexMethod::make_method(cls, dispatch_name, dispatch_proto);
}
#undef LOG_AND_RETURN

DexAccessFlags get_dispatch_access(DexMethod* origin_method) {
  auto method_type = possible_type(origin_method);
  if (method_type == dispatch::STATIC) {
    return ACC_STATIC | ACC_PUBLIC;
  } else if (method_type == dispatch::VIRTUAL) {
    return ACC_PUBLIC;
  } else {
    always_assert(method_type == dispatch::DIRECT);
    return ACC_PRIVATE;
  }
}

size_t get_type_tag_location_for_ctor_and_static(const dispatch::Spec& spec,
                                                 const DexTypeList* arg_list) {
  if (spec.type == dispatch::Type::CTOR) {
    if (spec.type_tag_param_idx) {
      // The local variable idx is param idx + 1 because of the first implicit
      // `this` argument to ctors.
      return *spec.type_tag_param_idx + 1;
    }
    // No type tag. Return a dummy value.
    return arg_list->size();
  } else if (spec.type == dispatch::Type::CTOR_SAVE_TYPE_TAG_PARAM) {
    return arg_list->size();
  } else if (spec.type == dispatch::Type::STATIC) {
    always_assert(is_static(spec.access_flags));
    return arg_list->size() - 1;
  }

  always_assert_log(false, "Unexpected dispatch type %d\n", spec.type);
}

} // namespace

namespace dispatch {

constexpr const char* DISPATCH_PREFIX = "$dispatch$";

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
  auto type_tag_loc = mc->get_local(
      get_type_tag_location_for_ctor_and_static(spec, dispatch_arg_list));
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
  handle_default_block(spec, indices_to_callee, args, mc, ret_loc, def_block);
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

// TODO(fengliu): There are some redundant logic with other dispatch creating
// methods, will do some refactor in near future.
DexMethod* create_simple_dispatch(
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee,
    DexAnnotationSet* anno,
    bool with_debug_item) {
  auto dispatch_ref = create_dispatch_method_ref(indices_to_callee);
  if (!dispatch_ref) {
    return nullptr;
  }
  auto return_type = dispatch_ref->get_proto()->get_rtype();
  auto first_method = indices_to_callee.begin()->second;
  auto access = get_dispatch_access(first_method);
  MethodCreator mc(dispatch_ref, access, nullptr, true);
  auto args = mc.get_reg_args();
  auto method_tag_loc = *args.rbegin();
  // The mc's last argument is a "method tag", pop the argument and pass the
  // rest to the mergeables.
  args.pop_back();
  auto main_block = mc.get_main_block();

  std::map<SwitchIndices, MethodBlock*> cases;
  for (auto& p : indices_to_callee) {
    cases[p.first] = nullptr;
  }
  auto default_block = main_block->switch_op(method_tag_loc, cases);
  bool has_return_value = (return_type != get_void_type());
  auto res_loc =
      has_return_value ? mc.make_local(return_type) : Location::empty();
  for (auto& p : cases) {
    auto case_block = p.second;
    auto callee = indices_to_callee.at(p.first);
    case_block->invoke(callee, args);
    if (has_return_value) {
      case_block->move_result(res_loc, return_type);
      case_block->ret(res_loc);
    } else {
      case_block->ret_void();
    }
  }

  auto method = mc.create();
  method->rstate = first_method->rstate;
  return method;
}

DexString* gen_dispatch_name(DexType* owner,
                             DexProto* proto,
                             std::string orig_name) {
  auto simple_name = DexString::make_string(DISPATCH_PREFIX + orig_name);
  if (DexMethod::get_method(owner, simple_name, proto) == nullptr) {
    return simple_name;
  }

  size_t count = 0;
  while (true) {
    auto suffix = "$" + std::to_string(count);
    auto dispatch_name = DexString::make_string(simple_name->str() + suffix);
    auto existing_meth = DexMethod::get_method(owner, dispatch_name, proto);
    if (existing_meth == nullptr) {
      return dispatch_name;
    }
    ++count;
  }
}

bool may_be_dispatch(const DexMethod* method) {
  const auto& name = method->str();
  if (name.find(DISPATCH_PREFIX) != 0) {
    return false;
  }
  auto code = method->get_code();
  uint32_t branches = 0;
  for (auto& mie : InstructionIterable(code)) {
    auto op = mie.insn->opcode();
    if (is_switch(op)) {
      return true;
    }
    branches += is_conditional_branch(op);
    if (branches > 1) {
      return true;
    }
  }
  return false;
}
} // namespace dispatch
