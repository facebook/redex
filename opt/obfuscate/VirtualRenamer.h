/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Obfuscate.h"

// Renames virtual methods avoiding conflicts up the class hierarchy and
// avoiding collisions of methods printed in a stack trace when
// avoid_stack_trace_collision is true
size_t rename_virtuals(
    Scope& scope,
    bool avoid_stack_trace_collision = false,
    const std::unordered_map<const DexClass*, int>& next_dmethod_seeds = {});
