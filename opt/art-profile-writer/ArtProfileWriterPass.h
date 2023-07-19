/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ArtProfileWriterPass : public Pass {
 public:
  ArtProfileWriterPass() : Pass("ArtProfileWriterPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoInitClassInstructions, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
        {RenameClass, Preserves},
    };
  }

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  struct PerfConfig {
    float appear100_threshold;
    float call_count_threshold;
    float coldstart_appear100_threshold;
    std::vector<std::string> interactions{"ColdStart"};

    PerfConfig()
        : appear100_threshold(101.0),
          call_count_threshold(0),
          coldstart_appear100_threshold(80.0) {} // Default: off
    PerfConfig(float a, float c, float ca)
        : appear100_threshold(a),
          call_count_threshold(c),
          coldstart_appear100_threshold(ca) {}
  };

  PerfConfig m_perf_config;
};
