/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RewriteKotlinSingletonInstance.h"

#include "ConfigFiles.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "LocalPointersAnalysis.h"
#include "PassManager.h"
#include "SideEffectSummary.h"
#include "SummarySerialization.h"

namespace {
bool check_inits_has_side_effects(
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    IRCode* code,
    const std::unordered_set<DexMethodRef*>& safe_base_invoke) {
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  auto iterable = cfg::InstructionIterable(cfg);
  side_effects::Summary summary(side_effects::EFF_NONE, {});
  side_effects::InvokeToSummaryMap summary_map;

  for (auto it = iterable.begin(); it != iterable.end(); it++) {
    auto insn = it->insn;
    if (insn->opcode() != OPCODE_INVOKE_DIRECT) {
      continue;
    }
    if (safe_base_invoke.count(insn->get_method())) {
      summary_map.emplace(insn, summary);
      continue;
    }
  }

  reaching_defs::MoveAwareFixpointIterator reaching_defs_iter(cfg);
  reaching_defs_iter.run({});

  local_pointers::FixpointIterator fp_iter(cfg);
  fp_iter.run({});

  auto side_effect_summary =
      side_effects::SummaryBuilder(init_classes_with_side_effects, summary_map,
                                   fp_iter, code, &reaching_defs_iter,
                                   /* analyze_external_reads */ true)
          .build();

  return side_effect_summary.is_pure();
}
bool init_for_type_has_side_effects(
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    DexClass* cls,
    const std::unordered_set<DexMethodRef*>& safe_base_invoke) {
  auto ifields = cls->get_ifields();
  if (!is_final(cls) || !ifields.empty()) {
    return true;
  }
  auto dmethods = cls->get_dmethods();
  DexMethod* init = nullptr;
  for (auto method : dmethods) {
    if (method::is_init(method)) {
      if (init) {
        // multiple init
        return true;
      }
      init = method;
    }
  }
  if (!init || !init->get_proto()->get_args()->empty() || !init->get_code()) {
    return true;
  }
  auto is_pure = check_inits_has_side_effects(
      init_classes_with_side_effects, init->get_code(), safe_base_invoke);
  return !is_pure;
}

} // namespace
void RewriteKotlinSingletonInstance::run_pass(DexStoresVector& stores,
                                              ConfigFiles& conf,
                                              PassManager& mgr) {
  auto lamda_base_invoke =
      DexMethod::get_method("Lkotlin/jvm/internal/Lambda;.<init>:(I)V");
  auto obj_init = DexMethod::get_method("Ljava/lang/Object;.<init>:()V");
  if (!lamda_base_invoke || !obj_init) {
    return;
  }
  std::unordered_set<DexMethodRef*> safe_base_invoke;
  safe_base_invoke.insert(lamda_base_invoke);
  safe_base_invoke.insert(obj_init);

  Scope scope = build_class_scope(stores);
  KotlinInstanceRewriter rewriter;

  ConcurrentMap<DexFieldRef*, std::set<std::pair<IRInstruction*, DexMethod*>>>
      concurrentLambdaMap;

  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns());
  auto do_not_consider_type = [&init_classes_with_side_effects,
                               &safe_base_invoke](DexClass* cls) -> bool {
    return init_for_type_has_side_effects(init_classes_with_side_effects, cls,
                                          safe_base_invoke);
  };
  KotlinInstanceRewriter::Stats stats = rewriter.collect_instance_usage(
      scope, concurrentLambdaMap, do_not_consider_type);
  stats += rewriter.remove_escaping_instance(scope, concurrentLambdaMap);
  stats += rewriter.transform(concurrentLambdaMap);
  stats.report(mgr);
}

static RewriteKotlinSingletonInstance s_pass;
