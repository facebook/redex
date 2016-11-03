/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <iostream>

#include "DexClass.h"
#include "DexUtil.h"

namespace redex {

void print_seeds(std::ostream& output,
                 const ProguardMap& pg_map,
                 const Scope& classes,
                 const bool allowshrinking_filter = false,
                 const bool allowobfuscation_filter = false);
}
