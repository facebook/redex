/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexStore.h"
#include "DexUtil.h"

using TypeSet = std::set<const DexType*, dextypes_comparator>;

// An API that takes a pre-constructed XStoreRefs to avoid recomputation, if
// called frequently.
bool is_in_non_root_store(const DexType* type,
                          const DexStoresVector& stores,
                          const XStoreRefs& xstores,
                          bool include_primary_dex);

std::unordered_set<const DexType*> get_non_root_store_types(
    const DexStoresVector& stores,
    const XStoreRefs& xstores,
    const TypeSet& types,
    bool include_primary_dex);

std::unordered_set<const DexType*> get_non_root_store_types(
    const DexStoresVector& stores,
    const TypeSet& types,
    bool include_primary_dex);
