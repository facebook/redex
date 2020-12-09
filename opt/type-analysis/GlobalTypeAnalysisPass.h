/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GlobalTypeAnalyzer.h"
#include "Pass.h"
#include "TypeAnalysisRuntimeAssert.h"
#include "TypeAnalysisTransform.h"

/*
 * A dummy pass that runs the global type analysis only without actually
 * consuming the result.
 */
class GlobalTypeAnalysisPass : public Pass {
 public:
  struct Config {
    size_t max_global_analysis_iteration{10};
    bool insert_runtime_asserts{false};
    bool trace_global_local_diff{false};
    type_analyzer::Transform::Config transform;
    type_analyzer::RuntimeAssertTransform::Config runtime_assert;
  };

  Config& get_config() { return m_config; }

  struct Stats {
    type_analyzer::Transform::Stats transform_stats;
    type_analyzer::RuntimeAssertTransform::Stats assert_stats;

    Stats& operator+=(const Stats& that) {
      transform_stats += that.transform_stats;
      assert_stats += that.assert_stats;
      return *this;
    }

    void report(PassManager& mgr) const {
      transform_stats.report(mgr);
      assert_stats.report(mgr);
    }
  };

  explicit GlobalTypeAnalysisPass(Config config)
      : Pass("GlobalTypeAnalysisPass", Pass::ANALYSIS), m_config(config) {}

  GlobalTypeAnalysisPass() : GlobalTypeAnalysisPass(Config()) {}

  void bind_config() override {
    bind("max_global_analysis_iteration", size_t(10),
         m_config.max_global_analysis_iteration,
         "Maximum number of global iterations the analysis runs");
    bind("insert_runtime_asserts", false, m_config.insert_runtime_asserts);
    bind("trace_global_local_diff", false, m_config.trace_global_local_diff);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void optimize(
      const Scope& scope,
      const type_analyzer::global::GlobalTypeAnalyzer& gta,
      const type_analyzer::Transform::NullAssertionSet& null_assertion_set,
      PassManager& mgr);

  std::shared_ptr<type_analyzer::global::GlobalTypeAnalyzer> get_result() {
    return m_result;
  }

  void destroy_analysis_result() override { m_result = nullptr; }

 private:
  Config m_config;
  std::shared_ptr<type_analyzer::global::GlobalTypeAnalyzer> m_result = nullptr;
};
