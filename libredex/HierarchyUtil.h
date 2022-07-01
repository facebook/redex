/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "DexClass.h"

namespace hierarchy_util {

/*
 * Returns all non-overridden virtual methods in scope, plus methods from
 * external classes. The external classes will be included even if they are not
 * in the input Scope parameter.
 */
std::unordered_set<const DexMethod*> find_non_overridden_virtuals(
    const Scope& scope);

} // namespace hierarchy_util
