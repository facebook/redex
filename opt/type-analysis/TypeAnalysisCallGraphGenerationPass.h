/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CallGraph.h"
#include "GlobalTypeAnalysisPass.h"
#include "Pass.h"

/*
 * The pass generates a version of call graph based on the result of the global
 * type analysis, and exports it to a subsequent consumer pass.
 */
class TypeAnalysisCallGraphGenerationPass : public Pass {
 public:
  struct Config {
    bool dump_call_graph{false};
  };

  Config& get_config() { return m_config; }

  explicit TypeAnalysisCallGraphGenerationPass(Config config)
      : Pass("TypeAnalysisCallGraphGenerationPass", Pass::ANALYSIS),
        m_config(config) {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
    };
  }

  TypeAnalysisCallGraphGenerationPass()
      : TypeAnalysisCallGraphGenerationPass(Config()) {}

  void bind_config() override {
    bind("dump_call_graph", false, m_config.dump_call_graph);
    trait(Traits::Pass::unique, true);
  }

  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<GlobalTypeAnalysisPass>();
    au.set_preserve_all();
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  std::shared_ptr<call_graph::Graph> get_result() { return m_result; }

  void destroy_analysis_result() override { m_result = nullptr; }

 private:
  Config m_config;
  std::shared_ptr<call_graph::Graph> m_result = nullptr;
};
