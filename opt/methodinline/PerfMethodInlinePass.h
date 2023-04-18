/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Pass.h"

class PerfMethodInlinePass : public Pass {
 public:
  PerfMethodInlinePass() : Pass("PerfMethodInlinePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{HasSourceBlocks, {.requires_ = true, .preserves = true}},
            {NoSpuriousGetClassCalls, {.preserves = true}}};
  }

  ~PerfMethodInlinePass();

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  struct Config;
  std::unique_ptr<Config> m_config;
};
