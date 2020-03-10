/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/*
 * A dummy pass that runs the global type analysis only without actually
 * consuming the result.
 */
class GlobalTypeAnalysisPass : public Pass {
 public:
  GlobalTypeAnalysisPass() : Pass("GlobalTypeAnalysisPass") {}

  void bind_config() override {
    bind("max_global_analysis_iteration", size_t(100),
         m_max_global_analysis_iteration,
         "Maximum number of global iterations the analysis runs");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_max_global_analysis_iteration;
};
