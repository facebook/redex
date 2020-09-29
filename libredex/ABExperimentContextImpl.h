/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ABExperimentContext.h"
#include "ControlFlow.h"

namespace ab_test {

enum class ABGlobalMode { CONTROL, TEST, NONE };

class ABExperimentContextImpl : public ABExperimentContext {
 public:
  void flush() override;

  ABExperimentContextImpl(cfg::ControlFlowGraph* cfg,
                          DexMethod* m,
                          ABExperimentPreferredMode preferred_mode);

  ~ABExperimentContextImpl() override;

 private:
  static void set_global_mode(ABGlobalMode ab_global_mode = ABGlobalMode::NONE);

  DexMethod* m_original_method{nullptr};
  cfg::ControlFlowGraph* m_cfg{nullptr};
  std::unique_ptr<cfg::ControlFlowGraph> m_cloned_cfg{nullptr};
  bool m_flushed{false};
  ABExperimentPreferredMode m_preferred_mode;

  bool use_test();
  void setup_context();

  friend class ABExperimentContext;
  friend class ABExperimentContextTest;
};
} // namespace ab_test
