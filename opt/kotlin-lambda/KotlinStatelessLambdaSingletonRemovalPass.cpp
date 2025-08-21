/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinStatelessLambdaSingletonRemovalPass.h"

#include "DexUtil.h"
#include "KotlinInstanceRewriter.h"
#include "TypeUtil.h"

namespace {

bool is_kotlin_stateless_lambda(const DexClass* cls) {
  return type::is_kotlin_non_capturing_lambda(cls);
}

} // namespace

void KotlinStatelessLambdaSingletonRemovalPass::run_pass(
    DexStoresVector& stores, ConfigFiles& conf, PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  KotlinInstanceRewriter rewriter;
  ConcurrentMap<DexFieldRef*, std::set<std::pair<IRInstruction*, DexMethod*>>>
      concurrentLambdaMap;

  auto do_not_consider_type = [](DexClass* cls) -> bool {
    return !is_kotlin_stateless_lambda(cls);
  };

  // TODO: Update the rewriter logic
  KotlinInstanceRewriter::Stats stats = rewriter.collect_instance_usage(
      scope, concurrentLambdaMap, do_not_consider_type);

  stats += rewriter.remove_escaping_instance(scope, concurrentLambdaMap);
  stats += rewriter.transform(concurrentLambdaMap);
  stats.report(mgr);
}

static KotlinStatelessLambdaSingletonRemovalPass s_pass;
