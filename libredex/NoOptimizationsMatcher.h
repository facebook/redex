/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConfigFiles.h"

namespace keep_rules {

/**
 * This method walks all methods in the given scope and looks for annotations
 * that match any of the given annotation types.
 *
 * After this processing, `method->rstate.no_optimization()` can be queried
 * to check whether a `method` matched.
 */
void process_no_optimizations_rules(
    const std::unordered_set<DexType*>& no_optimizations_annos,
    const Scope& scope);

} // namespace keep_rules
