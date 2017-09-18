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

#include "PassManager.h"

class RegAllocPass : public Pass {
 public:
  RegAllocPass() : Pass("RegAllocPass") {}
  virtual void configure_pass(const PassConfig& pc) override {
    if (!RedexContext::assume_regalloc()) {
      fprintf(stderr,
              "WARNING: you probably want to set `\"assume_regalloc\" : true` "
              "in the redex config\n");
    }
    pc.get("live_range_splitting", false, m_use_splitting);
  }
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_use_splitting = false;
};
