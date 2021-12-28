/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>

// Before Android 8, we cannot have more than 2 ^ 15 type refs in one dex.
// NOTE: This is because of a bug found in Android up to 7.
constexpr size_t kOldMaxTypeRefs = 1 << 15;
constexpr size_t kNewMaxTypeRefs = 1 << 16;
inline size_t get_max_type_refs(int min_sdk) {
  return min_sdk < 26 ? kOldMaxTypeRefs : kNewMaxTypeRefs;
}

// Methods and fields have the full 16-bit space available
constexpr size_t kMaxMethodRefs = 64 * 1024;
constexpr size_t kMaxFieldRefs = 64 * 1024;
