/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <sstream>

#include <json/json.h>

#include "CppUtil.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "Show.h"

namespace {
constexpr const char* ROOT_STORE_NAME = "classes";

DexStoresDependencies build_transitive_resolved_dependencies(
    const DexStoresVector& stores) {
  DexStoresDependencies transitive_resolved_dependencies;
  if (stores.size() == 1) {
    // special case to accomodate tests with non-standard store names
    auto& store = stores.front();
    transitive_resolved_dependencies.emplace(&store, DexStoreDependencies());
    return transitive_resolved_dependencies;
  }

  // We handle the root store separately, as it may appear twist in the list
  // of stores (a quick to handle the primary dex).
  auto& root_store = stores.front();
  always_assert_log(
      root_store.get_name() == ROOT_STORE_NAME,
      "Root store has name {%s}, but should be {%s}, out of %zu stores",
      root_store.get_name().c_str(), ROOT_STORE_NAME, stores.size());
  // For some reason, the root store is referenced by the name "dex" via
  // dependencies
  std::unordered_map<std::string, const DexStore*> named_stores;
  named_stores.emplace("dex", &root_store);
  for (auto& store : stores) {
    if (&store == &root_store) {
      continue;
    }
    auto emplaced = named_stores.emplace(store.get_name(), &store).second;
    always_assert_log(emplaced, "Duplicate store name: %s",
                      store.get_name().c_str());
  }

  std::function<const DexStoreDependencies&(const DexStore* store)> build;
  build = [&](const DexStore* store) -> const DexStoreDependencies& {
    auto it = transitive_resolved_dependencies.find(store);
    if (it == transitive_resolved_dependencies.end()) {
      DexStoreDependencies deps;
      for (auto& dependency_name : store->get_dependencies()) {
        auto it2 = named_stores.find(dependency_name);
        if (it2 == named_stores.end()) {
          // This routinely happens for some reason
          continue;
        }
        auto dependency_store = it2->second;
        deps.insert(dependency_store);
        const auto& deps_deps = build(dependency_store);
        deps.insert(deps_deps.begin(), deps_deps.end());
      }
      it = transitive_resolved_dependencies.emplace(store, std::move(deps))
               .first;
    }
    return it->second;
  };
  for (auto& store : stores) {
    build(&store);
  }
  return transitive_resolved_dependencies;
}

} // namespace

DexStore::DexStore(std::string name, std::vector<std::string> deps) {
  m_metadata.set_id(std::move(name));
  m_metadata.set_dependencies(std::move(deps));
}

const std::string& DexStore::get_name() const { return m_metadata.get_id(); }

bool DexStore::is_root_store() const {
  return m_metadata.get_id() == ROOT_STORE_NAME;
}

std::vector<DexClasses>& DexStore::get_dexen() { return m_dexen; }

const std::vector<DexClasses>& DexStore::get_dexen() const { return m_dexen; }

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

void DexStore::add_class(DexClass* new_cls,
                         DexStoresVector& stores,
                         boost::optional<size_t> dex_id) {
  redex_assert(!stores.empty());
  if (dex_id == boost::none) {
    DexClassesVector& root_store = stores.front().get_dexen();
    redex_assert(!root_store.empty());
    // Add the class to the last dex of root_store.
    root_store.back().emplace_back(new_cls);
  } else {
    size_t id = *dex_id;
    for (auto& store : stores) {
      auto& dexen = store.get_dexen();
      if (id < dexen.size()) {
        dexen[id].emplace_back(new_cls);
        return;
      }
      id -= dexen.size();
    }
    not_reached_log("Invalid dex_id %zu", *dex_id);
  }
}

void DexMetadata::parse(const std::string& path) {
  std::ifstream input(path);
  Json::Value store;
  input >> store;
  id = store["id"].asString();
  for (const auto& dep : store["requires"]) {
    dependencies.push_back(dep.asString());
  }
  for (const auto& file : store["files"]) {
    files.push_back(file.asString());
  }
}

std::unordered_set<const DexType*> get_root_store_types(
    const DexStoresVector& stores, bool include_primary_dex) {
  std::unordered_set<const DexType*> types;
  redex_assert(!stores.empty());
  const auto& root_dexen = stores[0].get_dexen();
  size_t index = include_primary_dex ? 0 : 1;
  for (; index < root_dexen.size(); index++) {
    for (const auto cls : root_dexen[index]) {
      types.insert(cls->get_type());
    }
  }
  return types;
}

XStoreRefs::XStoreRefs(const DexStoresVector& stores)
    : m_transitive_resolved_dependencies(
          build_transitive_resolved_dependencies(stores)) {
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

bool XStoreRefs::illegal_ref_load_types(const DexType* location,
                                        const DexClass* cls) const {
  std::unordered_set<DexType*> types;
  cls->gather_load_types(types);
  for (auto* t : types) {
    if (illegal_ref(location, t)) {
      return true;
    }
  }
  return false;
}

std::string XStoreRefs::show_type(const DexType* type) { return show(type); }

XDexRefs::XDexRefs(const DexStoresVector& stores) {
  size_t dex_nr = 0;
  for (auto& store : stores) {
    for (auto& dexen : store.get_dexen()) {
      for (const auto cls : dexen) {
        m_dexes.emplace(cls->get_type(), dex_nr);
      }
      dex_nr++;
    }
  }
  m_num_dexes = dex_nr;
}

size_t XDexRefs::get_dex_idx(const DexType* type) const {
  auto it = m_dexes.find(type);
  always_assert_log(it != m_dexes.end(), "type %s not in the current APK",
                    SHOW(type));
  return it->second;
}

bool XDexRefs::cross_dex_ref_override(const DexMethod* overridden,
                                      const DexMethod* overriding) const {
  auto type = overriding->get_class();
  auto idx = get_dex_idx(type);
  do {
    type = type_class(type)->get_super_class();
    if (idx != get_dex_idx(type)) {
      return true;
    }
  } while (type != overridden->get_class());
  return false;
}

bool XDexRefs::is_in_primary_dex(const DexMethod* method) const {
  return get_dex_idx(method->get_class()) == 0;
}

size_t XDexRefs::num_dexes() const { return m_num_dexes; }

bool XDexRefs::cross_dex_ref(const DexMethod* caller,
                             const DexMethod* callee) const {
  return get_dex_idx(callee->get_class()) != get_dex_idx(caller->get_class());
}

void squash_into_one_dex(DexStoresVector& stores) {
  redex_assert(!stores.empty());
  auto& root_store = *stores.begin();
  auto& dexes = root_store.get_dexen();
  if (dexes.empty()) {
    redex_assert(stores.size() == 1);
    return;
  }
  auto it = dexes.begin();
  auto& primary_dex = *it;
  for (it++; it != dexes.end(); ++it) {
    primary_dex.insert(primary_dex.end(), it->begin(), it->end());
  }
  dexes.erase(dexes.begin() + 1, dexes.end());
  for (auto other_store_it = ++stores.begin(); other_store_it != stores.end();
       ++other_store_it) {
    for (auto& dex : other_store_it->get_dexen()) {
      primary_dex.insert(primary_dex.end(), dex.begin(), dex.end());
    }
  }
  stores.erase(stores.begin() + 1, stores.end());
}

std::string dex_name(const DexStore& store, size_t dex_id) {
  std::ostringstream ss;
  ss << store.get_name();
  if (store.get_name().compare("classes") == 0) {
    // primary/secondary dex store, primary has no numeral and secondaries
    // start at 2
    if (dex_id > 0) {
      ss << (dex_id + 1);
    }
  } else {
    // other dex stores do not have a primary,
    // so it makes sense to start at 2
    ss << (dex_id + 2);
  }
  ss << ".dex";
  return ss.str();
}
