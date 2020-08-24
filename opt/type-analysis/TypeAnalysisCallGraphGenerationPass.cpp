/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeAnalysisCallGraphGenerationPass.h"

#include "DexUtil.h"
#include "GlobalTypeAnalysisPass.h"
#include "Walkers.h"

using namespace type_analyzer;

void TypeAnalysisCallGraphGenerationPass::run_pass(DexStoresVector& stores,
                                                   ConfigFiles& config,
                                                   PassManager& mgr) {
  auto analysis = mgr.get_preserved_analysis<GlobalTypeAnalysisPass>();
  always_assert(analysis);
  auto gta = analysis->get_result();
  always_assert(gta);
}

static TypeAnalysisCallGraphGenerationPass s_pass;
