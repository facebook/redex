/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

#define METRIC_COLD_START_SET_DEX_COUNT "cold_start_set_dex_count"

#define INTERDEX_PASS_NAME "InterDexPass"

class InterDexPass : public Pass {
 public:
  InterDexPass() : Pass(INTERDEX_PASS_NAME) {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("static_prune", false, m_static_prune);
    pc.get("emit_canaries", true, m_emit_canaries);
    pc.get("normal_primary_dex", false, m_normal_primary_dex);
    pc.get("linear_alloc_limit", 11600 * 1024, m_linear_alloc_limit);
  }

  virtual void run_pass(DexClassesVector&, ConfigFiles&, PassManager&);
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_static_prune;
  bool m_emit_canaries;
  bool m_normal_primary_dex;
  int64_t m_linear_alloc_limit;
};
