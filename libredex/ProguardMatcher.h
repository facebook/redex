/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"

namespace keep_rules {

using Scope = std::vector<DexClass*>;

ConcurrentSet<const KeepSpec*> process_proguard_rules(
    const ProguardMap& pg_map,
    const Scope& classes,
    const Scope& external_classes,
    const ProguardConfiguration& pg_config,
    bool keep_all_annotation_classes);
} // namespace keep_rules
