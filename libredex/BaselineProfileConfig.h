/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <json/json.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#include "DeterministicContainers.h"
#include "JsonWrapper.h"

namespace baseline_profiles {

constexpr const char* DEFAULT_BASELINE_PROFILE_CONFIG_NAME = "default";

// Note: Not everything is relevant to Redex in here. This should mostly 1:1
// map to the JSON config currently being passed to the baseline profile
// driver.

// TODO: These are all set to the same defaults which the baseline profile
//       driver sets for interactions. This should ideally be defined in one
//       external place and passed to both Redex and the driver.
struct BaselineProfileInteractionConfig {
  int64_t call_threshold = 1;
  bool classes = true;
  bool post_startup = true;
  bool startup = false;
  int64_t threshold = 80;
};

struct BaselineProfileHarvestConfig {
  bool enable_never_compile = false;
  int64_t never_compile_callcount_threshold = -1;
  int64_t never_compile_perf_threshold = -1;
  int64_t never_compile_called_coverage_threshold = -1;
  std::string never_compile_excluded_interaction_pattern;
  int64_t never_compile_excluded_appear100_threshold = 20;
  int64_t never_compile_excluded_call_count_threshold = 0;
  bool never_compile_ignore_hot = false;
  bool never_compile_strings_lookup_methods = false;
  void load_from_json(const Json::Value& json_input) {
    const auto& jw = JsonWrapper(json_input);
    jw.get("enable_never_compile", false, enable_never_compile);
    jw.get("never_compile_callcount_threshold", -1,
           never_compile_callcount_threshold);
    jw.get("never_compile_perf_threshold", -1, never_compile_perf_threshold);
    jw.get("never_compile_called_coverage_threshold", -1,
           never_compile_called_coverage_threshold);
    jw.get("never_compile_excluded_interaction_pattern", "",
           never_compile_excluded_interaction_pattern);
    jw.get("never_compile_excluded_appear100_threshold", 20,
           never_compile_excluded_appear100_threshold);
    jw.get("never_compile_excluded_call_count_threshold", 0,
           never_compile_excluded_call_count_threshold);
    jw.get("never_compile_ignore_hot", false, never_compile_ignore_hot);
    jw.get("never_compile_strings_lookup_methods", false,
           never_compile_strings_lookup_methods);
  }
};

struct BaselineProfileOptions {
  bool oxygen_modules;
  bool strip_classes;
  bool transitively_close_classes;
  bool use_redex_generated_profile;
  // This field isn't used currently by the driver. We currently pass a
  // `--betamap` flag to the driver to enable betamap 20% set inclusion, which
  // isn't ideal. TODO: The driver config JSON should be updated to use this.
  // TODO: Rename this "betamap_include_coldstart_1pct"
  bool include_betamap_20pct_coldstart;

  // NOTE: This requires that include_betamap_20pct_coldstart be set to have any
  // effect
  bool betamap_include_coldstart_1pct;

  // If this is true, then the ArtProfileWriter will insert all methods/classes
  // from the betamap into the baseline profile.
  bool include_all_startup_classes;
  bool use_final_redex_generated_profile;
};

struct BaselineProfileConfig {
  UnorderedMap<std::string, BaselineProfileInteractionConfig>
      interaction_configs;
  std::vector<std::pair<std::string, std::string>> interactions;
  BaselineProfileOptions options;
  BaselineProfileHarvestConfig harvest_config;
  std::vector<std::string> manual_files;
};

using BaselineProfileConfigMap =
    UnorderedMap<std::string, BaselineProfileConfig>;

} // namespace baseline_profiles
