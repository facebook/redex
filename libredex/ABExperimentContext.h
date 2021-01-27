/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"

struct RedexTest;

namespace ab_test {

/**
 * Preferred mode of an ABExperimentContext. Either versions of the code
 * (PREFER_CONTROL - unmodified, PREFER_TEST - modified) will be chosen,
 * unless force_test_mode() or force_control_mode() have previously been called.
 */
enum class ABExperimentPreferredMode { PREFER_CONTROL, PREFER_TEST };

/**
 * RAII object that controls whether mutations happening to a CFG after the
 * experiment context has been created and before it has been flushed will
 * actually be visible (i.e. applied) or not, depending on its setup.
 */
class ABExperimentContext {
  friend RedexTest;

 public:
  static std::unique_ptr<ABExperimentContext> create(
      cfg::ControlFlowGraph* cfg,
      DexMethod* m,
      const std::string& exp_name,
      ABExperimentPreferredMode preferred_mode =
          ABExperimentPreferredMode::PREFER_CONTROL);

  /**
   * Decide which version (CONTROL/TEST) of code will be applied and also clears
   * the CFG that was created by the constructor. The method's CFG
   * should not be used anymore after the context is flushed.
   */
  virtual void flush() = 0;

  virtual ~ABExperimentContext() {}

  /**
   * Disables A/B experiments in this build of Redex.
   * When flushed, experiments will use their preferred mode instead of
   * tracking both branches.
   * NOTE: this can only be used as long as no ABExperimentContext instance
   * exists.
   */
  static void disable_ab_experiments();

  /**
   * Forces all ABExperimentContext instances to apply changes happening
   * to the CFG when flushing. This should only be used at the beginning of
   * Redex tests, when a pass is being experimented with and its tests need to
   * avoid the control logic.
   * NOTE: this can only be used as long as no ABExperimentContext instance
   * exists.
   * TODO(T80501167): This is made public temporarily to solve an issue with a
   * failing test. Find a better solution for that.
   */
  static void force_test_mode();

 private:
  /**
   * Forces all ABExperimentContext instances to ignore changes happening
   * to the CFG when flushing. This should only be used at the beginning of
   * Redex tests. Redex developers should write tests for passes that will use
   * the CONTROL branch only, i.e. no actual mutation (i.e. change by the pass)
   * to the CFG is applied. The reason for that is to test the behaviour of the
   * experiment for the CONTROL group (which should be the same as if there was
   * no pass).
   * NOTE: this can only be used as long as no ABExperimentContext instance
   * exists.
   */
  static void force_control_mode();
};
} // namespace ab_test
