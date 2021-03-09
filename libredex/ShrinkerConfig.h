/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

namespace shrinker {

/**
 * The global shrinker config.
 */
struct ShrinkerConfig {
  bool run_const_prop{false};
  bool run_cse{false};
  bool run_copy_prop{false};
  bool run_local_dce{false};
  bool run_reg_alloc{false};
  bool run_dedup_blocks{false};

  // Internally used option that decides whether to compute pure methods with a
  // relatively expensive analysis over the scope
  bool compute_pure_methods{true};

  // Decide which functions to run register allocation on.
  std::string reg_alloc_random_forest;

  // Internally used option that decides whether to analyze constructors (only
  // relevant when using constant-propagaation); requires cfg to be built
  bool analyze_constructors{false};
};

} // namespace shrinker
