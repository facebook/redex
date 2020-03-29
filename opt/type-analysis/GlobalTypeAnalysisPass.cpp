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
                                      PassManager& /* mgr */) {
  Scope scope = build_class_scope(stores);
  global::GlobalTypeAnalysis analysis(m_config.max_global_analysis_iteration);
  auto gta = analysis.analyze(scope);
  optimize(scope, *gta);
}

void GlobalTypeAnalysisPass::optimize(
    const Scope& scope, const type_analyzer::global::GlobalTypeAnalyzer& gta) {
  if (m_config.remove_dead_null_check_insn) {
    m_transform_stats =
        walk::parallel::methods<type_analyzer::Transform::Stats>(
            scope, [&](DexMethod* method) {
              if (method->get_code() == nullptr) {
                return type_analyzer::Transform::Stats();
              }

              auto lta = gta.get_local_analysis(method);
              auto& code = *method->get_code();
              Transform tf(m_config);
              return tf.apply(*lta, &code);
            });
  }
}

static GlobalTypeAnalysisPass s_pass;
