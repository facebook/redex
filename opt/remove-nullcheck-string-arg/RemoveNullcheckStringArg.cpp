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
#include "PassManager.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

constexpr const char* CHECK_PARAM_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/"
    "Object;Ljava/lang/String;)V";
constexpr const char* CHECK_EXPR_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/"
    "lang/Object;Ljava/lang/String;)V";

constexpr const char* WRAPPER_CHECK_PARAM_NULL_METHOD = "$WrCheckParameter";
constexpr const char* WRAPPER_CHECK_EXPR_NULL_METHOD = "$WrCheckExpression";

constexpr const char* NEW_CHECK_PARAM_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter:(Ljava/lang/Object;)V";
constexpr const char* NEW_CHECK_EXPR_NULL_SIGNATURE =
    "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression:(Ljava/lang/Object;)V";
} // namespace

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
    auto local_stats = change_in_cfg(code->cfg(), transfer_map);
    code->clear_cfg();
    return local_stats;
  });

  stats.report(mgr);
}

bool RemoveNullcheckStringArg::setup(
    TransferMap& transfer_map, std::unordered_set<DexMethod*>& new_methods) {
  auto new_check_param_method =
      get_wrapper_method(CHECK_PARAM_NULL_SIGNATURE,
                         WRAPPER_CHECK_PARAM_NULL_METHOD,
                         NEW_CHECK_PARAM_NULL_SIGNATURE);
  auto new_check_expr_method =
      get_wrapper_method(CHECK_EXPR_NULL_SIGNATURE,
                         WRAPPER_CHECK_EXPR_NULL_METHOD,
                         NEW_CHECK_EXPR_NULL_SIGNATURE);

  /* If we could not generate suitable wrapper method, giveup. */
  if (!new_check_param_method || !new_check_expr_method) {
    return false;
  }
  transfer_map[DexMethod::get_method(CHECK_PARAM_NULL_SIGNATURE)] =
      new_check_param_method;
  transfer_map[DexMethod::get_method(CHECK_EXPR_NULL_SIGNATURE)] =
      new_check_expr_method;
  new_methods.insert(new_check_param_method);
  new_methods.insert(new_check_expr_method);
  return true;
}

/* If the \p wrapper_signature is already present or if the function being
 * wrapped does not exist or if creation of new method fails, return nullptr.
 * Otherwise create a method in the same class as in \p
 * builtin_signature with a new name \p wrapper_name. */
DexMethod* RemoveNullcheckStringArg::get_wrapper_method(
    const char* builtin_signature,
    const char* wrapper_name,
    const char* wrapper_signature) {

  if (DexMethod::get_method(wrapper_signature)) {
    /* Wrapper method already exist. */
    return nullptr;
  }

  DexMethodRef* builtin = DexMethod::get_method(builtin_signature);
  /* If we didn't find the method, giveup. */
  if (!builtin) {
    return nullptr;
  }

  auto host_cls = type_class(builtin->get_class());
  if (!host_cls) {
    return nullptr;
  }
  const auto proto = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::java_lang_Object()}));
  MethodCreator method_creator(host_cls->get_type(),
                               DexString::make_string(wrapper_name),
                               proto,
                               ACC_PUBLIC | ACC_STATIC);
  auto obj_arg = method_creator.get_local(0);
  auto str_var = method_creator.make_local(type::java_lang_String());
  auto main_block = method_creator.get_main_block();

  main_block->load_const(str_var, DexString::make_string(""));
  std::vector<Location> args;
  args.push_back(obj_arg);
  args.push_back(str_var);

  main_block->invoke(OPCODE_INVOKE_STATIC, builtin, args);
  main_block->ret_void();
  auto new_method = method_creator.create();
  host_cls->add_method(new_method);
  return new_method;
}

RemoveNullcheckStringArg::Stats RemoveNullcheckStringArg::change_in_cfg(
    cfg::ControlFlowGraph& cfg, const TransferMap& transfer_map) {
  Stats stats{};
  cfg::CFGMutation m(cfg);

  auto ii = InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    if (insn->opcode() != OPCODE_INVOKE_STATIC) {
      continue;
    }
    auto iter = transfer_map.find(insn->get_method());
    if (iter == transfer_map.end()) {
      continue;
    }
    IRInstruction* new_insn = new IRInstruction(OPCODE_INVOKE_STATIC);
    new_insn->set_method(iter->second)
        ->set_srcs_size(1)
        ->set_src(0, insn->src(0));
    m.replace(it, {new_insn});
    stats.null_check_insns_changed++;
  }

  m.flush();
  return stats;
}

void RemoveNullcheckStringArg::Stats::report(PassManager& mgr) const {
  mgr.incr_metric("null_check_insns_changed", null_check_insns_changed);
  TRACE(NULLCHECK, 2, "RemoveNullcheckStringArgPass Stats:");
  TRACE(NULLCHECK,
        2,
        "RemoveNullcheckStringArgPass insns changed = %u",
        null_check_insns_changed);
}

// Computes set of uninstantiable types, also looking at the type system to
// find non-external (and non-native)...
static RemoveNullcheckStringArg s_pass;
