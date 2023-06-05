/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Creators.h"
#include "DexStore.h"
#include "DexUtil.h"

constexpr const char* SECONDARY_CANARY_PREFIX = "Lsecondary/dex";
constexpr const char SECONDARY_CANARY_CLASS_FORMAT[] =
    "Lsecondary/dex%02d/Canary;";
constexpr size_t SECONDARY_CANARY_CLASS_BUFSIZE =
    sizeof(SECONDARY_CANARY_CLASS_FORMAT);

constexpr const char STORE_CANARY_CLASS_FORMAT[] = "Lstore%04x/dex%02d/Canary;";
constexpr size_t STORE_CANARY_CLASS_BUFSIZE = sizeof(STORE_CANARY_CLASS_FORMAT);

std::string get_canary_name(int dexnum, const DexString* store_name);
bool is_canary(DexClass* clazz);
DexClass* create_canary(int dexnum, const DexString* store_name = nullptr);

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
