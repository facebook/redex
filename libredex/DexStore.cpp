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

std::string DexStore::get_name() {
  return m_metadata.get_id();
}

std::vector<DexClasses>& DexStore::get_dexen() {
  return m_dexen;
}

std::vector<std::string> DexStore::get_dependencies() {
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
