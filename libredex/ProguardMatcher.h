/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"

namespace redex {

using Scope = std::vector<DexClass*>;

void process_proguard_rules(const ProguardMap& pg_map,
                            const ProguardConfiguration& pg_config,
                            Scope& classes);
}

// namespace redex
