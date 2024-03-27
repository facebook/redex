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

struct Stats {
  size_t num_eliminated_enum_classes{0};
  size_t num_eliminated_kotlin_enum_classes{0};
  size_t num_erased_enum_objs{0};
  size_t num_int_objs{0};
};

Stats transform_enums(PassManager& mgr,
                      const Config& config,
                      DexStoresVector* stores);

} // namespace optimize_enums
