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
 * RAII object that controls whether mutations happening to a CFG after the
 * experiment context has been created and before it has been flushed will
 * actually be visible (i.e. applied) or not, depending on its setup.
 */
class ABExperimentContext {
  friend RedexTest;

 public:
  static std::unique_ptr<ABExperimentContext> create(
      const std::string& exp_name);

  virtual void try_register_method(DexMethod* m) = 0;

  virtual bool use_control() = 0;
  virtual bool use_test() = 0;

  /**
   * Decide which version (CONTROL/TEST) of code will be applied and also clears
   * the CFG that was created by the constructor. The method's CFG
   * should not be used anymore after the context is flushed.
   */
  virtual void flush() = 0;

  virtual ~ABExperimentContext() {}

  static void parse_experiments_states(
      const std::unordered_map<std::string, std::string>& states, bool);

 private:
  static void reset_global_state();
};
} // namespace ab_test
