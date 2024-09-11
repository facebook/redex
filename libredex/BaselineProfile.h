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

#include "DexClass.h"

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

} // namespace baseline_profiles
