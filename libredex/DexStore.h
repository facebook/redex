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
#include <vector>
#include <unordered_set>
#include "DexClass.h"

class DexStore;
using DexStoresVector = std::vector<DexStore>;

class DexMetadata {
  std::string id;
  std::vector<std::string> dependencies;
  std::vector<std::string> files;

public:
  const std::string get_id() const { return id; }
  void set_id(std::string name) { id = name; }
  void set_files(const std::vector<std::string>& f) { files = f; }
  const std::vector<std::string> get_files() const { return files; }
  const std::vector<std::string> get_dependencies() const { return dependencies; }
  std::vector<std::string>& get_dependencies() { return dependencies; }

  void parse(const std::string& path);
};

class DexStore {
  std::vector<DexClasses> m_dexen;
  DexMetadata m_metadata;

 public:
  DexStore(const DexMetadata metadata) :
    m_metadata(metadata) {};
  DexStore(const std::string name);
  DexStore(const DexStore&) = delete;
  DexStore(DexStore&&) = default;

  std::string get_name() const;
  std::vector<DexClasses>& get_dexen();
  const std::vector<DexClasses>& get_dexen() const;
  std::vector<std::string> get_dependencies() const;

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

/**
 * Provide an API to determine whether an illegal cross DexStore
 * reference/dependency is created.
 * TODO: this probably need to rely on metadata to be correct. Right now it
 *       just uses order of DexStores.
 */
class XStoreRefs {
 private:
  /**
   * Set of classes in each logical store. A primary DEX goes in its own
   * bucket (first element in the array).
   */
  std::vector<std::unordered_set<const DexType*>> m_xstores;

 public:
  explicit XStoreRefs(const DexStoresVector& stores);

  /**
   * Return a stored idx for a given type.
   * The store idx can be used in the 'bool illegal_ref(size_t, const DexType*)'
   * api.
   */
  size_t get_store_idx(const DexType* type) const {
    for (size_t store_idx = 0; store_idx < m_xstores.size(); store_idx++) {
      if (m_xstores[store_idx].count(type) > 0) return store_idx;
    }
    always_assert_log(false, "type %s not in the current APK", SHOW(type));
  }

  /**
   * Verify that a 'type' can be moved in the DexStore where 'location' is
   * defined.
   * Use it for one time calls where 'type' is moved either in a method (member
   * in general) of 'location' or more broadly a reference to 'type' is made
   * in 'location'.
   */
  bool illegal_ref(const DexType* location, const DexType* type) const {
    return illegal_ref(get_store_idx(location), type);
  }

  /**
   * Verify that a 'type' can be moved in the DexStore identified by
   * 'store_idx'.
   * Use it when an anlaysis over a given DEX (or instructions in a given
   * method/class) needs to be performed by an optimization.
   */
  bool illegal_ref(size_t store_idx, const DexType* type) const {
    if (type_class_internal(type) == nullptr) return false;
    // Temporary HACK: optimizations may leave references to dead classes and
    // if we just call get_store_idx() - as we should - the assert will fire...
    size_t type_store_idx = 0;
    for (; type_store_idx < m_xstores.size(); type_store_idx++) {
      if (m_xstores[type_store_idx].count(type) > 0) break;
    }
    return type_store_idx > store_idx;
  }

};
