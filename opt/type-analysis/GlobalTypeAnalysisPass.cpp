/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalTypeAnalysisPass.h"

#include "DexUtil.h"
#include "GlobalTypeAnalyzer.h"

using namespace type_analyzer;

void GlobalTypeAnalysisPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* conf */,
                                      PassManager& /* mgr */) {
  Scope scope = build_class_scope(stores);
  global::GlobalTypeAnalysis analysis(m_max_global_analysis_iteration);
  analysis.analyze(scope);
}

static GlobalTypeAnalysisPass s_pass;
