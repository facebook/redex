/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstdio>

#include "GraphColoring.h"
#include "PassManager.h"

class RegAllocPass : public Pass {
 public:
  RegAllocPass() : Pass("RegAllocPass") {}
  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("live_range_splitting", false, m_allocator_config.use_splitting);
    pc.get("use_spill_costs", false, m_allocator_config.use_spill_costs);
  }
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  regalloc::graph_coloring::Allocator::Config m_allocator_config;
};
