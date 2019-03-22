/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdio>

#include "GraphColoring.h"
#include "PassManager.h"

class RegAllocPass : public Pass {
 public:
  RegAllocPass() : Pass("RegAllocPass") {}
  void configure_pass(const JsonWrapper& jw) override {
    jw.get("live_range_splitting", false, m_allocator_config.use_splitting);
    jw.get("use_spill_costs", false, m_allocator_config.use_spill_costs);
    jw.get("no_overwrite_this", false, m_allocator_config.no_overwrite_this);
  }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  regalloc::graph_coloring::Allocator::Config m_allocator_config;
};
