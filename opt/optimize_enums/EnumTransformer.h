/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexStore.h"
#include "EnumClinitAnalysis.h"
#include "EnumConfig.h"
#include "PassManager.h"

namespace optimize_enums {
int transform_enums(PassManager& mgr,
                    const Config& config,
                    DexStoresVector* stores,
                    size_t* num_int_objs);
} // namespace optimize_enums
