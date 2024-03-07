/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexStoreUtil.h"

bool is_canary(DexClass* clazz) {
  const char* cname = clazz->get_type()->get_name()->c_str();
  return strncmp(cname, SECONDARY_CANARY_PREFIX,
                 strlen(SECONDARY_CANARY_PREFIX)) == 0;
}

std::string get_canary_name(int dexnum, const DexString* store_name) {
  if (store_name) {
    char buf[STORE_CANARY_CLASS_BUFSIZE];
    int store_id = store_name->java_hashcode() & 0xFFFF;
    // Yes, there could be collisions. We assume that is handled outside of
    // Redex.
    snprintf(buf, sizeof(buf), STORE_CANARY_CLASS_FORMAT, store_id, dexnum + 1);
    return std::string(buf);
  } else {
    char buf[SECONDARY_CANARY_CLASS_BUFSIZE];
    snprintf(buf, sizeof(buf), SECONDARY_CANARY_CLASS_FORMAT, dexnum);
    return std::string(buf);
  }
}

DexClass* create_canary(int dexnum, const DexString* store_name) {
  std::string canary_name = get_canary_name(dexnum, store_name);
  auto canary_type = DexType::get_type(canary_name);
  if (!canary_type) {
    canary_type = DexType::make_type(canary_name);
  }
  auto canary_cls = type_class(canary_type);
  if (!canary_cls) {
    ClassCreator cc(canary_type);
    cc.set_access(ACC_PUBLIC | ACC_ABSTRACT);
    cc.set_super(type::java_lang_Object());
    canary_cls = cc.create();
    // Don't rename the Canary we've created
    canary_cls->rstate.set_keepnames();
    canary_cls->rstate.set_generated();
  }
  return canary_cls;
}

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

  always_assert(!stores.empty());
  XStoreRefs xstores(stores);
  return get_non_root_store_types(stores, xstores, types, include_primary_dex);
}
