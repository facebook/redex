/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstdlib>
#include <cstring>
#include "DexClass.h"

class DexClasses;

class DexStore {
  std::vector<DexClasses> m_dexen;
  std::string m_name;

 public:
  DexStore(const std::string name) :
    m_name(name) {
  };
  DexStore(const DexStore&) = delete;
  DexStore(DexStore&&) = default;

  std::string get_name();
  std::vector<DexClasses>& get_dexen();

  void add_classes(DexClasses classes);
};

class DexStoreClassesIterator : public std::iterator<std::input_iterator_tag, DexClasses> {

  using classes_iterator = std::vector<DexClasses>::iterator;
  using store_iterator = std::vector<DexStore>::iterator;

  std::vector<DexStore>& m_stores;
  store_iterator m_current_store;
  classes_iterator m_current_classes;

public:
  DexStoreClassesIterator(std::vector<DexStore>& stores) :
    m_stores(stores),
    m_current_store(stores.begin()),
    m_current_classes(m_current_store->get_dexen().begin()) { }

  DexStoreClassesIterator(const std::vector<DexStore>& stores) :
    m_stores(const_cast<std::vector<DexStore>&>(stores)),
    m_current_store(m_stores.begin()),
    m_current_classes(m_current_store->get_dexen().begin()) { }

  DexStoreClassesIterator(std::vector<DexStore>& stores, store_iterator current_store, classes_iterator current_classes) :
    m_stores(stores),
    m_current_store(current_store),
    m_current_classes(current_classes) { }

  DexStoreClassesIterator& operator++() {
    ++m_current_classes;
    while (m_current_store != m_stores.end() &&
           m_current_classes != m_stores.back().get_dexen().end() &&
           m_current_classes == m_current_store->get_dexen().end()) {
      ++m_current_store;
      m_current_classes = m_current_store->get_dexen().begin();
    }
    return *this;
  }

  DexStoreClassesIterator begin() const { return DexStoreClassesIterator(m_stores); };
  DexStoreClassesIterator end() const {
    return DexStoreClassesIterator(
      m_stores,
      m_stores.end(),
      m_stores.back().get_dexen().end());
  };

  bool operator==(const DexStoreClassesIterator& rhs) { return m_current_classes == rhs.m_current_classes; }
  bool operator!=(const DexStoreClassesIterator& rhs) { return m_current_classes != rhs.m_current_classes; }
  DexClasses& operator*() { return *m_current_classes; }
};

class DexMetadata {
  std::string id;
  std::vector<std::string> dependencies;
  std::vector<std::string> files;

public:
  const std::string& get_id() { return id; }
  const std::vector<std::string> get_files() { return files; }

  void parse(const std::string& path);
};
