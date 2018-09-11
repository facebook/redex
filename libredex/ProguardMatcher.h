/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"

namespace redex {

using Scope = std::vector<DexClass*>;

void process_proguard_rules(const ProguardMap& pg_map,
                            const Scope& classes,
                            const Scope& external_classes,
                            ProguardConfiguration* pg_config);
}

// namespace redex
