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
#include "MethodProfiles.h"
#include "TypeUtil.h"

namespace {

bool is_kotlin_stateless_lambda(const DexClass* cls) {
  return type::is_kotlin_non_capturing_lambda(cls);
}

bool is_hot_lambda(const DexClass* cls,
                   const method_profiles::MethodProfiles& method_profiles,
                   float call_count_threshold) {
  for (const auto* method : cls->get_vmethods()) {
    if (method->get_name()->str() == "invoke" &&
        method->get_code() != nullptr) {
      for (const auto& [interaction_id, stats_map] :
           method_profiles.all_interactions()) {
        auto it = stats_map.find(method);
        if (it != stats_map.end() &&
            it->second.call_count > call_count_threshold) {
          return true;
        }
      }
      break;
    }
  }
  return false;
}

} // namespace

void KotlinStatelessLambdaSingletonRemovalPass::run_pass(
    DexStoresVector& stores, ConfigFiles& conf, PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  KotlinInstanceRewriter rewriter;
  ConcurrentMap<DexFieldRef*, std::set<std::pair<IRInstruction*, DexMethod*>>>
      concurrentLambdaMap;

  const auto& method_profiles = conf.get_method_profiles();

  auto do_not_consider_type = [this, &method_profiles](DexClass* cls) -> bool {
    return !is_kotlin_stateless_lambda(cls) ||
           (m_exclude_hot && is_hot_lambda(cls,
                                           method_profiles,
                                           m_exclude_hot_call_count_threshold));
  };

  // TODO: Update the rewriter logic
  KotlinInstanceRewriter::Stats stats = rewriter.collect_instance_usage(
      scope, concurrentLambdaMap, do_not_consider_type);

  stats += rewriter.remove_escaping_instance(scope, concurrentLambdaMap);
  stats += rewriter.transform(concurrentLambdaMap);
  stats.report(mgr);
}

static KotlinStatelessLambdaSingletonRemovalPass s_pass;
