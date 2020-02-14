/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include "DexClass.h"

namespace max_depth {

std::unordered_map<const DexMethod*, int> analyze(const Scope& scope,
                                                  unsigned max_iteration = 20);

} // namespace max_depth
