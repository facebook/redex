/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdint.h>
#include <unordered_map>
#include <unordered_set>

#include "BaselineProfileConfig.h"
#include "DexClass.h"
#include "MethodProfiles.h"

namespace baseline_profiles {

struct MethodFlags {
  bool hot{false};
  bool startup{false};
  bool post_startup{false};
};

struct BaselineProfile {
  std::unordered_map<const DexMethod*, MethodFlags> methods;
  std::unordered_set<const DexClass*> classes;
};

std::unordered_map<std::string, BaselineProfile> get_baseline_profiles(
    const std::unordered_map<std::string, BaselineProfileConfig>& configs,
    const method_profiles::MethodProfiles& method_profiles,
    const bool ingest_baseline_profile_data,
    std::unordered_set<const DexMethodRef*>* method_refs_without_def = nullptr);

BaselineProfile get_default_baseline_profile(
    const std::unordered_map<std::string, BaselineProfileConfig>& configs,
    const method_profiles::MethodProfiles& method_profiles,
    const bool ingest_baseline_profile_data,
    std::unordered_set<const DexMethodRef*>* method_refs_without_def = nullptr);

} // namespace baseline_profiles
