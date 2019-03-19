/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <json/json.h>
#include <iostream>
#include <fstream>

#include "DexStore.h"
#include "DexUtil.h"

constexpr const char* ROOT_STORE_NAME = "classes";

DexStore::DexStore(const std::string name) {
  m_metadata.set_id(name);
}

std::string DexStore::get_name() const {
  return m_metadata.get_id();
}

bool DexStore::is_root_store() const {
  return m_metadata.get_id() == ROOT_STORE_NAME;
}

std::vector<DexClasses>& DexStore::get_dexen() {
  return m_dexen;
}

const std::vector<DexClasses>& DexStore::get_dexen() const {
  return m_dexen;
}

std::vector<std::string> DexStore::get_dependencies() const {
  return m_metadata.get_dependencies();
}

void DexStore::remove_classes(const DexClasses& classes) {
  std::unordered_set<DexClass*> to_remove(classes.begin(), classes.end());
  for (auto& dex_classes : m_dexen) {
    dex_classes.erase(std::remove_if(dex_classes.begin(),
                                     dex_classes.end(),
                                     [&](DexClass* cls) {
                                       return to_remove.count(cls) != 0;
                                     }),
                      dex_classes.end());
  }
}

void DexStore::add_classes(DexClasses classes) {
  m_dexen.push_back(std::move(classes));
}

void DexMetadata::parse(const std::string& path) {
  std::ifstream input(path);
  Json::Value store;
  input >> store;
  id = store["id"].asString();
  for (auto dep : store["requires"]) {
    dependencies.push_back(dep.asString());
  }
  for (auto file : store["files"]) {
    files.push_back(file.asString());
  }
}

XStoreRefs::XStoreRefs(const DexStoresVector& stores) {
  m_xstores.push_back(std::unordered_set<const DexType*>());
  m_stores.push_back(&stores[0]);
  for (const auto& cls : stores[0].get_dexen()[0]) {
    m_xstores.back().insert(cls->get_type());
  }
  m_root_stores = 1;
  if (stores[0].get_dexen().size() > 1) {
    m_root_stores++;
    m_xstores.push_back(std::unordered_set<const DexType*>());
    m_stores.push_back(&stores[0]);
    for (size_t i = 1; i < stores[0].get_dexen().size(); i++) {
      for (const auto& cls : stores[0].get_dexen()[i]) {
        m_xstores.back().insert(cls->get_type());
      }
    }
  }
  for (size_t i = 1; i < stores.size(); i++) {
    m_xstores.push_back(std::unordered_set<const DexType*>());
    m_stores.push_back(&stores[i]);
    for (const auto& classes : stores[i].get_dexen()) {
      for (const auto& cls : classes) {
        m_xstores.back().insert(cls->get_type());
      }
    }
  }
}

XDexRefs::XDexRefs(const DexStoresVector& stores) {
  for (size_t store_idx = 0; store_idx < stores.size(); ++store_idx) {
    auto& store = stores[store_idx];
    for (size_t id = 0; id < store.get_dexen().size(); id++) {
      m_dexes.push_back(std::unordered_set<const DexType*>());
      auto& dex = m_dexes.back();
      for (const auto cls : store.get_dexen()[id]) {
        dex.insert(cls->get_type());
      }
    }
  }
}

size_t XDexRefs::get_dex_idx(const DexType* type) const {
  for (size_t dex_idx = 0; dex_idx < m_dexes.size(); dex_idx++) {
    if (m_dexes[dex_idx].count(type) > 0) return dex_idx;
  }
  always_assert_log(false, "type %s not in the current APK", SHOW(type));
}

bool XDexRefs::cross_dex_ref(const DexMethod* caller,
                             const DexMethod* callee) const {
  size_t dex_idx = get_dex_idx(callee->get_class());
  return m_dexes[dex_idx].count(caller->get_class()) == 0;
}
