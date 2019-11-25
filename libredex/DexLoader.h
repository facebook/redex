/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexDefs.h"
#include "DexIdx.h"
#include "DexStats.h"
#include "DexUtil.h"

DexClasses load_classes_from_dex(const char* location,
                                 bool balloon = true,
                                 bool support_dex_v37 = false);
DexClasses load_classes_from_dex(const char* location,
                                 dex_stats_t* stats,
                                 bool balloon = true,
                                 bool support_dex_v37 = false);
DexClasses load_classes_from_dex(const dex_header* dh,
                                 const char* location,
                                 bool balloon = true);
const std::string load_dex_magic_from_dex(const char* location);
void balloon_for_test(const Scope& scope);

static inline const uint8_t* align_ptr(const uint8_t* const ptr,
                                       const size_t alignment) {
  const size_t alignment_error = ((size_t)ptr) % alignment;
  if (alignment_error != 0) {
    return ptr - alignment_error + alignment;
  } else {
    return ptr;
  }
}
