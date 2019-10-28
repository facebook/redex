/*
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

  void bind_config() override {
    bind("max_passes", {5}, m_pass_config.max_passes);
    bind("synth_only", false, m_pass_config.synth_only);
    bind("remove_pub", true, m_pass_config.remove_pub);
    bind("remove_constructors", true, m_pass_config.remove_constructors);
    bind("black_list_types", {}, m_pass_config.black_list_types);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  SynthConfig m_pass_config;
};
