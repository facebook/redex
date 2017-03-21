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

class Outliner : public Pass {
 public:
  Outliner() : Pass("Outliner") {}

  virtual void configure_pass(const PassConfig& pc) override {
    // N.B. we pretty much never want to outline the primary dex, but
    // we need to allow this to happen in some scenarios, e.g.
    // instrumentation tests, since they are single-dex affairs.
    pc.get("outline_primary_dex", false, m_outline_primary_dex);
  }

  virtual void run_pass(DexStoresVector& stores,
                        ConfigFiles& cfg,
                        PassManager& mgr) override;

 private:
  bool m_outline_primary_dex;
};
