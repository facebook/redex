/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include "DexClass.h"

namespace method_fixup {

void fixup_references_to_removed_methods(
    const Scope& scope,
    std::unordered_map<DexMethodRef*, DexMethodRef*>& removed_vmethods);

} // namespace method_fixup
