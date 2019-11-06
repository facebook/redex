/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "MethodProfiles.h"

namespace inline_for_speed {

using namespace method_profiles;

std::unordered_set<const DexMethodRef*> compute_hot_methods(
    const std::unordered_map<const DexMethodRef*, Stats>& method_profile_stats);

bool should_inline(const DexMethod* caller_method,
                   const DexMethod* callee_method,
                   const std::unordered_set<const DexMethodRef*>& hot_methods);

} // namespace inline_for_speed
