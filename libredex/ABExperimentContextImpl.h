/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ABExperimentContext.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"

enum class ABExperimentState;

namespace ab_test {

class ABExperimentContextImpl : public ABExperimentContext {
 public:
  void flush() override;

  explicit ABExperimentContextImpl(const std::string& exp_name);

  void try_register_method(DexMethod* /* unused */) override {}

  bool use_control() override;
  bool use_test() override;

  ~ABExperimentContextImpl() override {}

  static void parse_experiments_states(ConfigFiles& conf);
  static std::unordered_set<std::string> get_all_experiments_names();

 private:
  ABExperimentState m_state;

  bool m_flushed{false};

  static void reset_global_state();

  friend struct ABExperimentContextTest;
  friend class ABExperimentContext;
};
} // namespace ab_test
