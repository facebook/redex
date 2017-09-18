/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "DexIdx.h"
#include "DexDefs.h"
#include "DexUtil.h"

DexClasses load_classes_from_dex(const char* location, bool balloon = true);
DexClasses load_classes_from_dex(const char* location, dex_stats_t* stats, bool balloon = true);

void balloon_for_test(const Scope& scope);
