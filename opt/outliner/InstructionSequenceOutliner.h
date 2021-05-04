/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

namespace instruction_sequence_outliner {

enum class PerfSensitivity : char {
  // Never derive limits from perf sensitivity.
  kNeverUse,
  // Treat all perf-sensitive methods as "warm" when no method profiles are
  // given.
  kWarmWhenNoProfiles,
  // Treat all perf-sensitive methods as "hot" when no method profiles are
  // given.
  kHotWhenNoProfiles,
  // Treat all perf-sensitive methods as "warm."
  kAlwaysWarm,
  // Treat all perf-sensitive methods as "hot."
  kAlwaysHot,
};

struct Config {
  size_t min_insns_size{3};
  size_t max_insns_size{77};
  bool use_method_profiles{true};
  float method_profiles_appear_percent{1};
  float method_profiles_hot_call_count{10};
  float method_profiles_warm_call_count{1};
  PerfSensitivity perf_sensitivity{PerfSensitivity::kAlwaysHot};
  bool reorder_with_method_profiles{true};
  bool reuse_outlined_methods_across_dexes{true};
  size_t max_outlined_methods_per_class{100};
  size_t savings_threshold{10};
  bool outline_from_primary_dex{false};
  bool full_dbg_positions{false};
  bool debug_make_crashing{false};
};

} // namespace instruction_sequence_outliner

class InstructionSequenceOutliner : public Pass {
 public:
  InstructionSequenceOutliner() : Pass("InstructionSequenceOutlinerPass") {}

  void bind_config() override;
  void run_pass(DexStoresVector& stores,
                ConfigFiles& config,
                PassManager& mgr) override;

 private:
  instruction_sequence_outliner::Config m_config;
  size_t m_iteration{0};
};
