/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalTypeAnalysisPass.h"

#include "DexUtil.h"
#include "GlobalTypeAnalyzer.h"
#include "TypeAnalysisTransform.h"
#include "Walkers.h"

using namespace type_analyzer;

void GlobalTypeAnalysisPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* conf */,
                                      PassManager& mgr) {
  type_analyzer::Transform::NullAssertionSet null_assertion_set;
  Transform::setup(null_assertion_set);
  Scope scope = build_class_scope(stores);
  global::GlobalTypeAnalysis analysis(m_config.max_global_analysis_iteration);
  auto gta = analysis.analyze(scope);
  optimize(scope, *gta, null_assertion_set, mgr);
}

void GlobalTypeAnalysisPass::optimize(
    const Scope& scope,
    const type_analyzer::global::GlobalTypeAnalyzer& gta,
    const type_analyzer::Transform::NullAssertionSet& null_assertion_set,
    PassManager& mgr) {
  auto stats = walk::parallel::methods<type_analyzer::Transform::Stats>(
      scope, [&](DexMethod* method) {
        if (method->get_code() == nullptr) {
          return type_analyzer::Transform::Stats();
        }
        auto code = method->get_code();
        auto lta = gta.get_local_analysis(method);

        if (m_config.insert_runtime_asserts) {
          RuntimeAssertTransform rat(m_config.runtime_assert);
          rat.apply(*lta, gta.get_whole_program_state(), method);
          return Transform::Stats();
        }

        Transform tf(m_config.transform);
        auto local_stats = tf.apply(*lta, code, null_assertion_set);
        return local_stats;
      });
  stats.report(mgr);
}

static GlobalTypeAnalysisPass s_pass;
