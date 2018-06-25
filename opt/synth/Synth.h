/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
