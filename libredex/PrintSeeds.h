/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iostream>

#include "DexClass.h"
#include "DexUtil.h"

namespace keep_rules {

void print_seeds(std::ostream& output,
                 const ProguardMap& pg_map,
                 const Scope& classes,
                 const bool allowshrinking_filter = false,
                 const bool allowobfuscation_filter = false);

} // namespace keep_rules
