/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace outliner {

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

struct ProfileGuidanceConfig {
  bool use_method_profiles{true};
  bool enable_hotness_propagation{false};
  float method_profiles_appear_percent{1};
  float method_profiles_hot_call_count{10};
  float method_profiles_warm_call_count{1};
  PerfSensitivity perf_sensitivity{PerfSensitivity::kAlwaysHot};
  float block_profiles_hits{-1};
};

} // namespace outliner
