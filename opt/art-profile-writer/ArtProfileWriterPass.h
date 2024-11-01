/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

class ArtProfileWriterPass : public Pass {
 public:
  ArtProfileWriterPass() : Pass("ArtProfileWriterPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    return redex_properties::simple::preserves_all();
  }

  void bind_config() override;
  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  struct PerfConfig {
    float appear100_threshold;
    float call_count_threshold;
    float coldstart_appear100_threshold;
    // Threshold to include a coldstart method in the profile,
    // but not as a hot method. This threshold must be lower than
    // coldstart_appear100_threshold.
    float coldstart_appear100_nonhot_threshold;
    std::vector<std::string> interactions{"ColdStart"};

    PerfConfig()
        : appear100_threshold(101.0),
          call_count_threshold(0),
          coldstart_appear100_threshold(80.0),
          coldstart_appear100_nonhot_threshold(50.0) {} // Default: off
    PerfConfig(float a, float c, float ca, float can)
        : appear100_threshold(a),
          call_count_threshold(c),
          coldstart_appear100_threshold(ca),
          coldstart_appear100_nonhot_threshold(can) {}
  };

  PerfConfig m_perf_config;
  bool m_never_inline_estimate;
  bool m_never_inline_attach_annotations;
  int64_t m_never_compile_callcount_threshold;
  int64_t m_never_compile_perf_threshold;
  bool m_legacy_mode;
  std::string m_never_compile_excluded_interaction_pattern;
  int64_t m_never_compile_excluded_appear100_threshold;
  int64_t m_never_compile_excluded_call_count_threshold;
  std::optional<ReserveRefsInfoHandle> m_reserved_refs_handle;
};
