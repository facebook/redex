/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ObfuscatePass : public Pass {
 public:
  ObfuscatePass() : Pass("ObfuscatePass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void bind_config() final { trait(Traits::Pass::unique, true); }

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
  size_t classes_made_public{0};
};

void obfuscate(Scope& classes,
               RenameStats& stats,
               const ObfuscatePass::Config& config);
