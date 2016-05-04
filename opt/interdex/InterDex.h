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

class InterDexPass : public Pass {
 public:
  InterDexPass() : Pass("InterDexPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("static_prune", false, m_static_prune);
    pc.get("emit_canaries", true, m_emit_canaries);
  }

  virtual void run_pass(DexClassesVector&, ConfigFiles&) override;

 private:
  bool m_static_prune;
  bool m_emit_canaries;
};
