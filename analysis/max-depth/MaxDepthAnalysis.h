/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include "DexClass.h"
#include "Pass.h"

class MaxDepthAnalysisPass : public Pass {
 public:
  MaxDepthAnalysisPass() : Pass("MaxDepthAnalysisPass", Pass::ANALYSIS) {}
  void bind_config() override { bind("max_iteration", 20U, m_max_iteration); }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  using Result = std::unordered_map<const DexMethod*, int>;

  std::shared_ptr<Result> get_result() { return m_result; }

  void destroy_analysis_result() override { m_result = nullptr; }

 private:
  unsigned m_max_iteration;
  std::shared_ptr<Result> m_result = nullptr;
};
