/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "PassManager.h"
#include "DexClass.h"
#include "SynthConfig.h"

class SynthPass : public Pass {
 public:
  SynthPass() : Pass("SynthPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("max_passes", 5, m_pass_config.max_passes);
    pc.get("synth_only", false, m_pass_config.synth_only);
    pc.get("remove_pub", true, m_pass_config.remove_pub);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  SynthConfig m_pass_config;
};
