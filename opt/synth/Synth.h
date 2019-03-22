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

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("max_passes", 5, m_pass_config.max_passes);
    jw.get("synth_only", false, m_pass_config.synth_only);
    jw.get("remove_pub", true, m_pass_config.remove_pub);
    jw.get("remove_constructors", true, m_pass_config.remove_constructors);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  SynthConfig m_pass_config;
};
