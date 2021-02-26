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

enum class ABGlobalMode { CONTROL, TEST, NONE };

class ABExperimentContextImpl : public ABExperimentContext {
 public:
  void flush() override;

  ABExperimentContextImpl(cfg::ControlFlowGraph* cfg,
                          DexMethod* m,
                          const std::string& exp_name,
                          ABExperimentPreferredMode preferred_mode);

  ~ABExperimentContextImpl() override;

 private:
  static void set_global_mode(ABGlobalMode ab_global_mode = ABGlobalMode::NONE);
  static void parse_experiments_states(
      const std::unordered_map<std::string, std::string>& states);

  DexMethod* m_original_method{nullptr};
  cfg::ControlFlowGraph* m_cfg{nullptr};
  std::unique_ptr<cfg::ControlFlowGraph> m_cloned_cfg{nullptr};
  bool m_flushed{false};
  ABExperimentPreferredMode m_preferred_mode;
  ABExperimentState m_state;

  bool use_test();
  void setup_context();

  friend class ABExperimentContext;
  friend class ABExperimentContextTest;
};
} // namespace ab_test
