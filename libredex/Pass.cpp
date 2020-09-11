/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Pass.h"

#include "AnalysisUsage.h"
#include "Debug.h"
#include "DexUtil.h"
#include "PassRegistry.h"

Pass::Pass(const std::string& name, Kind kind) : m_name(name), m_kind(kind) {
  PassRegistry::get().register_pass(this);
}

void Pass::set_analysis_usage(AnalysisUsage& analysis_usage) const {
  // By default, analysis passes preserves all existing analysis while
  // transformation passes preserves none.
  if (m_kind == ANALYSIS) {
    analysis_usage.set_preserve_all();
  }
}

void PartialPass::run_pass(DexStoresVector& whole_program_stores,
                           ConfigFiles& conf,
                           PassManager& mgr) {
  Scope current_scope =
      build_class_scope_with_packages_config(whole_program_stores);
  run_partial_pass(whole_program_stores, std::move(current_scope), conf, mgr);
}

Scope PartialPass::build_class_scope_with_packages_config(
    const DexStoresVector& stores) {
  if (m_select_packages.empty()) {
    return build_class_scope(stores);
  } else {
    return build_class_scope_for_packages(stores, m_select_packages);
  }
}
