/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexDefs.h"
#include "DexIdx.h"
#include "DexUtil.h"

DexClasses load_classes_from_dex(const char* location, bool balloon = true);
DexClasses load_classes_from_dex(const char* location,
                                 dex_stats_t* stats,
                                 bool balloon = true);

void balloon_for_test(const Scope& scope);
