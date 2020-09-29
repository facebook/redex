/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdio>

#include "GraphColoring.h"
#include "PassManager.h"

namespace regalloc {

class RegAllocPass : public Pass {
 public:
  RegAllocPass() : Pass("RegAllocPass") {}

  void bind_config() override {
    bool unused;
    bind("live_range_splitting", false, unused);
    trait(Traits::Pass::atleast, 1);
  }

  /*
   * Allocate the code in a single method; exposed for unit tests.
   */
  static graph_coloring::Allocator::Stats allocate(
      const graph_coloring::Allocator::Config&, DexMethod*);

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};

} // namespace regalloc
