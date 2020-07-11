/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <vector>

class DexStore;
using DexStoresVector = std::vector<DexStore>;

/**
 * Remove small identical virtual methods.
 * 1. Consider non-root and renamable methods without invoke-super. Remove the
 * child method if it overrides a method with the same implementation.
 *
 * Example:
 * class P {
 *   public int method() { return 0; }
 * }
 * class Child extends P {
 *   public int method() { return 0; }
 * }
 */
namespace dedup_vmethods {
uint32_t dedup(const DexStoresVector& stores);
} // namespace dedup_vmethods
