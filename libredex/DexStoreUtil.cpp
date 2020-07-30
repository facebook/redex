/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexStoreUtil.h"

bool is_in_non_root_store(const DexType* type,
                          const DexStoresVector& stores,
                          const XStoreRefs& xstores,
                          bool include_primary_dex) {
  size_t store_idx = xstores.get_store_idx(type);
  // Hack to go around the fact that the primary dex goes in its own bucket.
  size_t next_store_idx = stores[0].get_dexen().size() == 1 ? 1 : 2;
  if (!include_primary_dex && store_idx == 0) {
    return true;
  } else if (store_idx >= next_store_idx) {
    return true;
  }
  return false;
}

std::unordered_set<const DexType*> get_non_root_store_types(
    const DexStoresVector& stores,
    const XStoreRefs& xstores,
    const TypeSet& types,
    bool include_primary_dex) {
  std::unordered_set<const DexType*> non_root_store_types;
  for (const DexType* type : types) {
    if (is_in_non_root_store(type, stores, xstores, include_primary_dex)) {
      non_root_store_types.emplace(type);
    }
  }
  return non_root_store_types;
}

std::unordered_set<const DexType*> get_non_root_store_types(
    const DexStoresVector& stores,
    const TypeSet& types,
    bool include_primary_dex) {

  always_assert(stores.size());
  XStoreRefs xstores(stores);
  return get_non_root_store_types(stores, xstores, types, include_primary_dex);
}
