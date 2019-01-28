/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "DexStore.h"

namespace optimize_enums {
int transform_enums(const ConcurrentSet<DexType*>& candidate_enums,
                    DexStoresVector* stores,
                    size_t* num_int_objs);
} // namespace optimize_enums
