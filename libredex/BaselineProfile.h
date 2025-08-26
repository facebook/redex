/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdint.h>

#include "BaselineProfileConfig.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "MethodProfiles.h"

struct ConfigFiles;

namespace baseline_profiles {

struct MethodFlags {
  bool hot{false};
  bool startup{false};
  bool post_startup{false};
};

struct BaselineProfile {
  UnorderedMap<const DexMethod*, MethodFlags> methods;
  UnorderedSet<const DexClass*> classes;

  // For output purposes. DO NOT USE DIRECTLY.
  UnorderedSet<std::string> unmatched_classes;
  size_t mark{0};

  void load_classes(const Scope& scope,
                    const ConfigFiles& config,
                    const std::string& bp_name);

  void transitively_close_classes(const Scope& scope);
};

// Returns a tuple of BaselineProfile and UnorderedMap<std::string,
// BaselineProfile> The first is the default profile that will be fed into the
// baseline profile driver as a manual input. The second is a mapping of config
// name to final baseline profile for every baseline profile that redex is
// generating.
std::tuple<BaselineProfile, UnorderedMap<std::string, BaselineProfile>>
get_baseline_profiles(
    const Scope& scope,
    const UnorderedMap<std::string, BaselineProfileConfig>& configs,
    const method_profiles::MethodProfiles& method_profiles,
    UnorderedSet<const DexMethodRef*>* method_refs_without_def = nullptr);

BaselineProfile get_default_baseline_profile(
    const Scope& scope,
    const UnorderedMap<std::string, BaselineProfileConfig>& configs,
    const method_profiles::MethodProfiles& method_profiles,
    UnorderedSet<const DexMethodRef*>* method_refs_without_def = nullptr);

} // namespace baseline_profiles
