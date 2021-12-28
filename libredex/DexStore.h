/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Debug.h"
#include "DexClass.h"

class DexStore;
using DexStoresVector = std::vector<DexStore>;

class DexMetadata {
  std::string id;
  std::vector<std::string> dependencies;
  std::vector<std::string> files;

 public:
  const std::string& get_id() const { return id; }
  void set_id(std::string name) { id = std::move(name); }
  void set_files(const std::vector<std::string>& f) { files = f; }
  const std::vector<std::string>& get_files() const { return files; }
  const std::vector<std::string>& get_dependencies() const {
    return dependencies;
  }
  std::vector<std::string>& get_dependencies() { return dependencies; }

  void parse(const std::string& path);
};

class DexStore {
  std::vector<DexClasses> m_dexen;
  DexMetadata m_metadata;
  std::string dex_magic = "";
  bool m_generated = false;

 public:
  explicit DexStore(const DexMetadata& metadata) : m_metadata(metadata){};
  explicit DexStore(const std::string& name);

  std::string get_name() const;
  const std::string& get_dex_magic() const { return dex_magic; }
  void set_dex_magic(const std::string& input_dex_magic) {
    dex_magic = input_dex_magic;
  }
  std::vector<DexClasses>& get_dexen();
  const std::vector<DexClasses>& get_dexen() const;
  std::vector<std::string> get_dependencies() const;
  bool is_root_store() const;
  bool is_longtail_store() const;

  void set_generated() { m_generated = true; }
  bool is_generated() const { return m_generated; }

  void remove_classes(const DexClasses& classes);
  void add_classes(DexClasses classes);

  /**
   * Add a class to the dex stores. If dex_id is none, add the class to the last
   * dex of root_store.
   */
  static void add_class(DexClass* new_cls,
                        DexStoresVector& stores,
                        boost::optional<size_t> dex_id);
};

class DexStoreClassesIterator
    : public std::iterator<std::input_iterator_tag, DexClasses> {

  using classes_iterator = std::vector<DexClasses>::iterator;
  using store_iterator = std::vector<DexStore>::iterator;

  std::vector<DexStore>& m_stores;
  store_iterator m_current_store;
  classes_iterator m_current_classes;

 public:
  explicit DexStoreClassesIterator(std::vector<DexStore>& stores)
      : m_stores(stores),
        m_current_store(stores.begin()),
        m_current_classes(m_current_store->get_dexen().begin()) {
    advance_end_classes();
  }

  explicit DexStoreClassesIterator(const std::vector<DexStore>& stores)
      : m_stores(const_cast<std::vector<DexStore>&>(stores)),
        m_current_store(m_stores.begin()),
        m_current_classes(m_current_store->get_dexen().begin()) {
    advance_end_classes();
  }

  DexStoreClassesIterator(std::vector<DexStore>& stores,
                          store_iterator current_store,
                          classes_iterator current_classes)
      : m_stores(stores),
        m_current_store(current_store),
        m_current_classes(current_classes) {
    advance_end_classes();
  }

  DexStoreClassesIterator& operator++() {
    ++m_current_classes;
    advance_end_classes();
    return *this;
  }

  DexStoreClassesIterator begin() const {
    return DexStoreClassesIterator(m_stores);
  };
  DexStoreClassesIterator end() const {
    return DexStoreClassesIterator(m_stores, m_stores.end(),
                                   m_stores.back().get_dexen().end());
  };

  bool operator==(const DexStoreClassesIterator& rhs) {
    return m_current_classes == rhs.m_current_classes;
  }
  bool operator!=(const DexStoreClassesIterator& rhs) {
    return m_current_classes != rhs.m_current_classes;
  }
  DexClasses& operator*() { return *m_current_classes; }

 private:
  void advance_end_classes() {
    while (m_current_store != m_stores.end() &&
           m_current_classes != m_stores.back().get_dexen().end() &&
           m_current_classes == m_current_store->get_dexen().end()) {
      ++m_current_store;
      m_current_classes = m_current_store->get_dexen().begin();
    }
  }
};

/**
 * Return all the root store types if `include_primary_dex` is true, otherwise
 * return all the types from secondary dexes.
 */
std::unordered_set<const DexType*> get_root_store_types(
    const DexStoresVector& stores, bool include_primary_dex = true);

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

  /**
   * Pointers to original stores in the same order as used to populate
   * m_xstores
   */
  std::vector<const DexStore*> m_stores;

  /**
   * Number of root stores.
   */
  size_t m_root_stores;

  static std::string show_type(const DexType* type); // To avoid "Show.h" in the
                                                     // header.

 public:
  explicit XStoreRefs(const DexStoresVector& stores);

  /**
   * If there's no secondary dexes, it returns 0. Otherwise it returns 1.
   */
  size_t largest_root_store_id() const { return m_root_stores - 1; }

  /**
   * Return a stored idx for a given type.
   * The store idx can be used in the 'bool illegal_ref(size_t, const DexType*)'
   * api.
   */
  size_t get_store_idx(const DexType* type) const {
    for (size_t store_idx = 0; store_idx < m_xstores.size(); store_idx++) {
      if (m_xstores[store_idx].count(type) > 0) return store_idx;
    }
    not_reached_log("type %s not in the current APK", show_type(type).c_str());
  }

  /**
   * Returns true if the class is in the root store. False if not.
   *
   * NOTE: False will be returned also for the cases where the type is not in
   * the current scope.
   */
  bool is_in_root_store(const DexType* type) const {
    for (size_t store_idx = 0; store_idx < m_root_stores; store_idx++) {
      if (m_xstores[store_idx].count(type) > 0) {
        return true;
      }
    }

    return false;
  }

  bool is_in_primary_dex(const DexType* type) const {
    return !m_xstores.empty() && m_xstores[0].count(type);
  }

  const DexStore* get_store(size_t idx) const { return m_stores[idx]; }

  const DexStore* get_store(const DexType* type) const {
    return m_stores[get_store_idx(type)];
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

  // Similar to the above, but correctly checks the class hierarchy. This
  // may be expensive, and only includes the classes that are guaranteed
  // to be resolved when the given class is loaded, but not further.
  bool illegal_ref_load_types(const DexType* location,
                              const DexClass* cls) const;

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
    if ((store_idx >= m_xstores.size()) ||
        (type_store_idx >= m_xstores.size())) {
      return type_store_idx > store_idx;
    }
    return illegal_ref_between_stores(store_idx, type_store_idx);
  }

  bool illegal_ref_between_stores(size_t caller_store_idx,
                                  size_t callee_store_idx) const {
    if (caller_store_idx == callee_store_idx) {
      return false;
    }

    bool callee_in_root_store = callee_store_idx < m_root_stores;

    if (callee_in_root_store) {
      // Check if primary to secondary reference
      return callee_store_idx > caller_store_idx;
    }

    // Check if the caller depends on the callee,
    // TODO - do it transitively.
    if (caller_store_idx >= m_root_stores) {
      const auto& callee_store_name = get_store(callee_store_idx)->get_name();
      const auto& caller_dependencies =
          get_store(caller_store_idx)->get_dependencies();

      if (std::find(caller_dependencies.begin(), caller_dependencies.end(),
                    callee_store_name) != caller_dependencies.end()) {
        return false;
      }
    }

    return true;
  }

  bool cross_store_ref(const DexMethod* caller, const DexMethod* callee) const {
    size_t store_idx = get_store_idx(caller->get_class());
    return illegal_ref(store_idx, callee->get_class());
  }

  size_t size() const { return m_xstores.size(); }
};

/**
 * We can not increase method references of any dex after interdex. The XDexRefs
 * is used for quick validation for crossing-dex references.
 */
class XDexRefs {
  std::unordered_map<const DexType*, size_t> m_dexes;
  size_t m_num_dexes;

 public:
  explicit XDexRefs(const DexStoresVector& stores);

  size_t get_dex_idx(const DexType* type) const;

  /**
   * Return true if the caller and callee are in different dexes.
   */
  bool cross_dex_ref(const DexMethod* caller, const DexMethod* callee) const;

  /**
   * Return true if the overridden and overriding methods, or any of the
   * intermediate classes in the inheritance hierarchy, are in different dexes.
   * The two methods must be non-interface virtual methods in the same virtual
   * scope, where the overriding method is defined in a (possibly nested)
   * sub-class of the class where the overridden method is defined.
   */
  bool cross_dex_ref_override(const DexMethod* overridden,
                              const DexMethod* overriding) const;

  /**
   * Return true if the method is located in the primary dex.
   */
  bool is_in_primary_dex(const DexMethod* method) const;

  /**
   * Number of dexes.
   */
  size_t num_dexes() const;
};

/**
 * Squash the stores into a single dex.
 */
void squash_into_one_dex(DexStoresVector& stores);

/**
 * Generate the name of the dex in format "${store_name}${new_id}.dex".
 * Primary dex has no numeral `new_id`. Secondaries and other dex stores do not
 * have a primary and their `new_id` start at 2.
 * Example: classes.dex, classes2.dex, classes3.dex, secondstore2.dex.
 */
std::string dex_name(const DexStore& store, size_t dex_id);
