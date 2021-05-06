/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveNullcheckStringArg.h"

#include "CFGMutation.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "KotlinNullCheckMethods.h"
#include "PassManager.h"
#include "ReachingDefinitions.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

using namespace kotlin_nullcheck_wrapper;

void RemoveNullcheckStringArg::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /*conf*/,
                                        PassManager& mgr) {
  TransferMap transfer_map;

  std::unordered_set<DexMethod*> new_methods;
  if (!setup(transfer_map, new_methods)) {
    TRACE(NULLCHECK, 2, "RemoveNullcheckStringArgPass: setup failed");
    return;
  }

  Scope scope = build_class_scope(stores);
  Stats stats = walk::parallel::methods<Stats>(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (method->rstate.no_optimizations() || code == nullptr ||
        new_methods.count(method)) {
      return Stats();
    }
    code->build_cfg();
    auto local_stats =
        change_in_cfg(code->cfg(), transfer_map, method->is_virtual());
    code->clear_cfg();
    return local_stats;
  });

  stats.report(mgr);
}

bool RemoveNullcheckStringArg::setup(
    TransferMap& transfer_map, std::unordered_set<DexMethod*>& new_methods) {

  DexMethodRef* builtin_param_V1_3 =
      DexMethod::get_method(CHECK_PARAM_NULL_SIGNATURE_V1_3);
  DexMethodRef* builtin_param_V1_4 =
      DexMethod::get_method(CHECK_PARAM_NULL_SIGNATURE_V1_4);
  DexMethodRef* builtin_expr_V1_3 =
      DexMethod::get_method(CHECK_EXPR_NULL_SIGNATURE_V1_3);
  DexMethodRef* builtin_expr_V1_4 =
      DexMethod::get_method(CHECK_EXPR_NULL_SIGNATURE_V1_4);

  if (builtin_param_V1_3) {
    auto new_check_param_method = get_wrapper_method_with_int_index(
        NEW_CHECK_PARAM_NULL_SIGNATURE_V1_3,
        WRAPPER_CHECK_PARAM_NULL_METHOD_V1_3, builtin_param_V1_3);
    if (new_check_param_method) {
      transfer_map[builtin_param_V1_3] =
          std::make_pair(new_check_param_method, true);
      new_methods.insert(new_check_param_method);
    }
  }
  if (builtin_param_V1_4) {
    auto new_check_param_method = get_wrapper_method_with_int_index(
        NEW_CHECK_PARAM_NULL_SIGNATURE_V1_4,
        WRAPPER_CHECK_PARAM_NULL_METHOD_V1_4, builtin_param_V1_4);
    if (new_check_param_method) {
      transfer_map[builtin_param_V1_4] =
          std::make_pair(new_check_param_method, true);
      new_methods.insert(new_check_param_method);
    }
  }

  if (builtin_expr_V1_3) {
    auto new_check_expr_method = get_wrapper_method(
        NEW_CHECK_EXPR_NULL_SIGNATURE_V1_3, WRAPPER_CHECK_EXPR_NULL_METHOD_V1_3,
        builtin_expr_V1_3);
    if (new_check_expr_method) {
      transfer_map[builtin_expr_V1_3] =
          std::make_pair(new_check_expr_method, false);
      new_methods.insert(new_check_expr_method);
    }
  }

  if (builtin_expr_V1_4) {
    auto new_check_expr_method = get_wrapper_method(
        NEW_CHECK_EXPR_NULL_SIGNATURE_V1_4, WRAPPER_CHECK_EXPR_NULL_METHOD_V1_4,
        builtin_expr_V1_4);
    if (new_check_expr_method) {
      transfer_map[builtin_expr_V1_4] =
          std::make_pair(new_check_expr_method, false);
      new_methods.insert(new_check_expr_method);
    }
  }

  /* If we could not generate suitable wrapper method, giveup. */
  if (new_methods.empty()) {
    return false;
  }

  return true;
}

DexMethod* RemoveNullcheckStringArg::get_wrapper_method(
    const char* wrapper_signature,
    const char* wrapper_name,
    DexMethodRef* builtin) {

  if (DexMethod::get_method(wrapper_signature)) {
    /* Wrapper method already exist. */
    return nullptr;
  }

  auto* host_cls = type_class(builtin->get_class());
  if (!host_cls || host_cls->is_external()) {
    return nullptr;
  }

  DexTypeList* arg_signature =
      DexTypeList::make_type_list({type::java_lang_Object()});
  const auto proto = DexProto::make_proto(type::_void(), arg_signature);
  MethodCreator method_creator(host_cls->get_type(),
                               DexString::make_string(wrapper_name),
                               proto,
                               ACC_PUBLIC | ACC_STATIC);
  auto obj_arg = method_creator.get_local(0);

  auto main_block = method_creator.get_main_block();
  auto if_block = main_block->if_testz(OPCODE_IF_NEZ, obj_arg);
  auto str_type = DexType::get_type("Ljava/lang/String;");
  if (!str_type) {
    return nullptr;
  }

  auto str_const = method_creator.make_local(str_type);

  // const-string v2, "UNKNOWN"
  if_block->load_const(str_const, DexString::make_string("UNKNOWN"));

  if_block->invoke(OPCODE_INVOKE_STATIC, builtin, {obj_arg, str_const});
  if_block->ret_void();
  main_block->ret_void();

  auto new_method = method_creator.create();
  TRACE(NULLCHECK, 5, "Created Method : %s", SHOW(new_method->get_code()));
  host_cls->add_method(new_method);
  return new_method;
}

DexMethod* RemoveNullcheckStringArg::get_wrapper_method_with_int_index(
    const char* wrapper_signature,
    const char* wrapper_name,
    DexMethodRef* builtin) {

  if (DexMethod::get_method(wrapper_signature)) {
    /* Wrapper method already exist. */
    return nullptr;
  }

  auto host_cls = type_class(builtin->get_class());
  if (!host_cls) {
    return nullptr;
  }

  DexTypeList* arg_signature =
      DexTypeList::make_type_list({type::java_lang_Object(), type::_int()});
  const auto proto = DexProto::make_proto(type::_void(), arg_signature);
  MethodCreator method_creator(host_cls->get_type(),
                               DexString::make_string(wrapper_name),
                               proto,
                               ACC_PUBLIC | ACC_STATIC);
  auto obj_arg = method_creator.get_local(0);

  // If the wrapper is going to print the index of the param as a string, we
  // will have to construct a string from the index with additional
  // information as part of the wrapper method.
  auto main_block = method_creator.get_main_block();
  auto int_ind = method_creator.get_local(1);
  auto str_type = DexType::get_type("Ljava/lang/String;");
  auto str_builder_type = DexType::get_type("Ljava/lang/StringBuilder;");
  if (!str_type || !str_builder_type) {
    return nullptr;
  }

  auto to_str_method = DexMethod::get_method(
      "Ljava/lang/Integer;.toString:(I)Ljava/lang/String;");
  auto str_builder_init_method =
      DexMethod::get_method("Ljava/lang/StringBuilder;.<init>:()V");
  auto append_method = DexMethod::get_method(
      "Ljava/lang/StringBuilder;.append:(Ljava/lang/"
      "String;)Ljava/lang/StringBuilder;");
  auto str_builder_to_str_method = DexMethod::get_method(
      "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;");

  if (!to_str_method || !append_method || !str_builder_to_str_method) {
    return nullptr;
  }
  auto str_ind = method_creator.make_local(str_type);
  auto str_builder = method_creator.make_local(str_builder_type);
  auto str_const = method_creator.make_local(str_type);
  auto str_res = method_creator.make_local(str_type);
  auto if_block = main_block->if_testz(OPCODE_IF_NEZ, obj_arg);

  // invoke-static {v3}, Ljava/lang/Integer;.toString:(I)Ljava/lang/String;
  if_block->invoke(OPCODE_INVOKE_STATIC, to_str_method, {int_ind});
  // move-result-object v3
  if_block->move_result(str_ind, str_type);
  // new-instance v1, Ljava/lang/StringBuilder;
  if_block->new_instance(str_builder_type, str_builder);
  // invoke-direct {v1}, Ljava/lang/StringBuilder;.<init>:()V
  if_block->invoke(OPCODE_INVOKE_DIRECT, str_builder_init_method,
                   {str_builder});
  // const-string v2, "param index = "
  if_block->load_const(str_const, DexString::make_string("param at index = "));
  // invoke-virtual {v1, v2},
  // Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;
  if_block->invoke(OPCODE_INVOKE_VIRTUAL, append_method,
                   {str_builder, str_const});
  // invoke-virtual {v1, v3},
  // Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;
  if_block->invoke(OPCODE_INVOKE_VIRTUAL, append_method,
                   {str_builder, str_ind});
  // invoke-virtual {v1},
  // Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;
  if_block->invoke(OPCODE_INVOKE_VIRTUAL, str_builder_to_str_method,
                   {str_builder});
  // move-result-object v3
  if_block->move_result(str_res, str_type);

  if_block->invoke(OPCODE_INVOKE_STATIC, builtin, {obj_arg, str_res});
  if_block->ret_void();
  main_block->ret_void();

  auto new_method = method_creator.create();
  TRACE(NULLCHECK, 5, "Created Method : %s", SHOW(new_method->get_code()));
  host_cls->add_method(new_method);
  return new_method;
}

RemoveNullcheckStringArg::Stats RemoveNullcheckStringArg::change_in_cfg(
    cfg::ControlFlowGraph& cfg,
    const TransferMap& transfer_map,
    bool is_virtual) {
  Stats stats{};
  cfg::CFGMutation m(cfg);
  auto params = cfg.get_param_instructions();
  std::unordered_map<size_t, uint32_t> param_index;
  uint32_t arg_index = is_virtual ? -1 : 0;

  reaching_defs::MoveAwareFixpointIterator reaching_defs_iter(cfg);
  reaching_defs_iter.run({});

  for (const auto& mie : InstructionIterable(params)) {
    auto load_insn = mie.insn;
    always_assert(opcode::is_a_load_param(load_insn->opcode()));
    param_index.insert(std::make_pair(load_insn->dest(), arg_index++));
  }

  for (cfg::Block* block : cfg.blocks()) {
    auto env = reaching_defs_iter.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end();
         reaching_defs_iter.analyze_instruction(it++->insn, &env)) {
      auto insn = it->insn;
      if (insn->opcode() != OPCODE_INVOKE_STATIC) {
        continue;
      }
      auto iter = transfer_map.find(insn->get_method());
      if (iter == transfer_map.end()) {
        continue;
      }
      IRInstruction* new_insn = new IRInstruction(OPCODE_INVOKE_STATIC);
      if (iter->second.second) {
        // We could have params copied via intermediate registers.
        auto defs = env.get(insn->src(0));
        always_assert(!defs.is_bottom() && !defs.is_top());
        always_assert(defs.elements().size() == 1);
        auto def = *defs.elements().begin();
        auto def_op = def->opcode();
        always_assert(def_op == IOPCODE_LOAD_PARAM ||
                      def_op == IOPCODE_LOAD_PARAM_OBJECT ||
                      def_op == IOPCODE_LOAD_PARAM_OBJECT);
        auto param_iter = param_index.find(def->dest());
        always_assert(param_iter != param_index.end());
        auto index = param_iter->second;
        auto tmp_reg = cfg.allocate_temp();
        IRInstruction* cst_insn = new IRInstruction(OPCODE_CONST);
        cst_insn->set_literal(index)->set_dest(tmp_reg);
        new_insn->set_method(iter->second.first)
            ->set_srcs_size(2)
            ->set_src(0, insn->src(0))
            ->set_src(1, tmp_reg);
        m.replace(cfg.find_insn(insn), {cst_insn, new_insn});
      } else {
        new_insn->set_method(iter->second.first)
            ->set_srcs_size(1)
            ->set_src(0, insn->src(0));
        m.replace(cfg.find_insn(insn), {new_insn});
      }
      stats.null_check_insns_changed++;
    }
  }

  m.flush();
  return stats;
}

void RemoveNullcheckStringArg::Stats::report(PassManager& mgr) const {
  mgr.incr_metric("null_check_insns_changed", null_check_insns_changed);
  TRACE(NULLCHECK, 2, "RemoveNullcheckStringArgPass Stats:");
  TRACE(NULLCHECK,
        2,
        "RemoveNullcheckStringArgPass insns changed = %zu",
        null_check_insns_changed);
}

// Computes set of uninstantiable types, also looking at the type system to
// find non-external (and non-native)...
static RemoveNullcheckStringArg s_pass;
