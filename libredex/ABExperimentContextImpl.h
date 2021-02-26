/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ABExperimentContext.h"
#include "ControlFlow.h"

enum class ABExperimentState;

namespace ab_test {

class ABExperimentContextImpl : public ABExperimentContext {
 public:
  void flush() override;

  ABExperimentContextImpl(cfg::ControlFlowGraph* cfg,
                          DexMethod* m,
                          const std::string& exp_name);

  ~ABExperimentContextImpl() override;

  static void parse_experiments_states(
      const std::unordered_map<std::string, std::string>& states);

 private:
  ABExperimentState m_state;

  DexMethod* m_original_method{nullptr};
  cfg::ControlFlowGraph* m_cfg{nullptr};
  std::unique_ptr<cfg::ControlFlowGraph> m_cloned_cfg{nullptr};
  bool m_flushed{false};

  bool use_test();
  void setup_context();
  static void reset_global_state();

  friend struct ABExperimentContextTest;
  friend class ABExperimentContext;
};
} // namespace ab_test
