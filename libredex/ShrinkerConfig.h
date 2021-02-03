/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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
};

} // namespace shrinker
