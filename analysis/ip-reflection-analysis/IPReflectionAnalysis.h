/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Pass.h"
#include "ReflectionAnalysis.h"

class IPReflectionAnalysisPass : public Pass {
 public:
  IPReflectionAnalysisPass()
      : Pass("IPReflectionAnalysisPass", Pass::ANALYSIS) {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
    };
  }

  void bind_config() override {
    bind("max_iteration", 20U, m_max_iteration);
    bind("export_results", false, m_export_results,
         "Generate redex-reflection-analysis.txt file containing the analysis "
         "results.");
  }
  bool is_cfg_legacy() override { return true; }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  using Result =
      std::unordered_map<const DexMethod*, reflection::ReflectionSites>;

  std::shared_ptr<Result> get_result() { return m_result; }

  void destroy_analysis_result() override { m_result = nullptr; }

 private:
  unsigned m_max_iteration;
  bool m_export_results;
  std::shared_ptr<Result> m_result;
};
