/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GlobalTypeAnalyzer.h"
#include "Pass.h"
#include "TypeAnalysisTransform.h"

/*
 * A dummy pass that runs the global type analysis only without actually
 * consuming the result.
 */
class GlobalTypeAnalysisPass : public Pass {
 public:
  explicit GlobalTypeAnalysisPass(type_analyzer::Transform::Config config)
      : Pass("GlobalTypeAnalysisPass"), m_config(config) {}

  GlobalTypeAnalysisPass()
      : GlobalTypeAnalysisPass(type_analyzer::Transform::Config()) {}

  void bind_config() override {
    bind("max_global_analysis_iteration", size_t(100),
         m_config.max_global_analysis_iteration,
         "Maximum number of global iterations the analysis runs");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void optimize(
      const Scope& scope,
      const type_analyzer::global::GlobalTypeAnalyzer& gta,
      const type_analyzer::Transform::NullAssertionSet& null_assertion_set,
      PassManager& mgr);

 private:
  type_analyzer::Transform::Config m_config;
};
