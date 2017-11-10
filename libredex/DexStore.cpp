/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include <json/json.h>
#include <iostream>
#include <fstream>

#include "DexStore.h"
#include "DexUtil.h"

DexStore::DexStore(const std::string name) {
  m_metadata.set_id(name);
}

std::string DexStore::get_name() const {
  return m_metadata.get_id();
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
  for (const auto& cls : stores[0].get_dexen()[0]) {
    m_xstores.back().insert(cls->get_type());
  }
  if (stores[0].get_dexen().size() > 1) {
    m_xstores.push_back(std::unordered_set<const DexType*>());
    for (size_t i = 1; i < stores[0].get_dexen().size(); i++) {
      for (const auto& cls : stores[0].get_dexen()[i]) {
        m_xstores.back().insert(cls->get_type());
      }
    }
  }
  for (size_t i = 1; i < stores.size(); i++) {
    m_xstores.push_back(std::unordered_set<const DexType*>());
    for (const auto& classes : stores[i].get_dexen()) {
      for (const auto& cls : classes) {
        m_xstores.back().insert(cls->get_type());
      }
    }
  }
}
