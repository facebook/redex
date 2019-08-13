/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "DexClass.h"
#include "DexStore.h"
#include "EnumClinitAnalysis.h"
#include "EnumConfig.h"
#include "MethodOverrideGraph.h"

namespace optimize_enums {
int transform_enums(const Config& config,
                    DexStoresVector* stores,
                    std::unique_ptr<const method_override_graph::Graph> graph,
                    size_t* num_int_objs);
} // namespace optimize_enums
