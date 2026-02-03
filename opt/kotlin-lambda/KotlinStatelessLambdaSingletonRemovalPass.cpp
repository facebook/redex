/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinStatelessLambdaSingletonRemovalPass.h"

#include "ConfigFiles.h"
#include "DexUtil.h"
#include "KotlinInstanceRewriter.h"
#include "KotlinLambdaAnalyzer.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "TypeUtil.h"

bool KotlinStatelessLambdaSingletonRemovalPass::is_hot_lambda(
    const KotlinLambdaAnalyzer& analyzer,
    const method_profiles::MethodProfiles& method_profiles) const {
  const auto* invoke = analyzer.get_invoke_method();
  if (invoke == nullptr) {
    return false;
  }
  for (const auto& [interaction_id, stats_map] :
       method_profiles.all_interactions()) {
    auto it = stats_map.find(invoke);
    if (it != stats_map.end() &&
        it->second.call_count > m_exclude_hot_call_count_threshold) {
      return true;
    }
  }
  return false;
}

void KotlinStatelessLambdaSingletonRemovalPass::run_pass(
    DexStoresVector& stores, ConfigFiles& conf, PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  KotlinInstanceRewriter rewriter;
  ConcurrentMap<DexFieldRef*, std::set<std::pair<IRInstruction*, DexMethod*>>>
      concurrentLambdaMap;

  const auto& method_profiles = conf.get_method_profiles();
  const bool has_method_profiles = method_profiles.has_stats();

  auto is_excludable =
      [this, &method_profiles, has_method_profiles](DexClass* cls) -> bool {
    auto analyzer = KotlinLambdaAnalyzer::for_class(cls);
    always_assert(analyzer && analyzer->is_non_capturing());
    return has_method_profiles && is_hot_lambda(*analyzer, method_profiles);
  };

  KotlinInstanceRewriter::Stats stats = rewriter.collect_instance_usage(
      scope, concurrentLambdaMap, is_excludable, m_exclude_hot);

  stats += rewriter.remove_escaping_instance(scope, concurrentLambdaMap);
  stats += rewriter.transform(concurrentLambdaMap);
  stats.report(mgr);
}

static KotlinStatelessLambdaSingletonRemovalPass s_pass;
