/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "PassManager.h"

class ObfuscatePass : public Pass {
 public:
  ObfuscatePass() : Pass("ObfuscatePass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("avoid_colliding_debug_name", false,
           m_config.avoid_colliding_debug_name);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Config {
    bool avoid_colliding_debug_name{false};
  };

 private:
  Config m_config;
};

struct RenameStats {
  size_t fields_total = 0;
  size_t fields_renamed = 0;
  size_t dmethods_total = 0;
  size_t dmethods_renamed = 0;
  size_t vmethods_total = 0;
  size_t vmethods_renamed = 0;
};

void obfuscate(Scope& classes,
               RenameStats& stats,
               const ObfuscatePass::Config& config);
