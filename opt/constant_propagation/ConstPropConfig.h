/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <unordered_set>
#include "DexClass.h"

struct ConstPropConfig {
  std::unordered_set<DexType*> blacklist;
  bool replace_moves_with_consts{false};
  bool fold_arithmetic{false};
  bool propagate_conditions{false};
  bool include_virtuals{false};
  bool dynamic_input_checks{false};
};
