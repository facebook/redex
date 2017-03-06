/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <iostream>

#include "Tool.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "JarLoader.h"
#include "ReachableClasses.h"

namespace fs = boost::filesystem;

namespace {

void load_store_dexen(DexStore& store, DexMetadata& store_metadata) {
  for (const auto& file_path : store_metadata.get_files()) {
    std::cout << "Loading " << file_path << std::endl;
    DexClasses classes = load_classes_from_dex(file_path.c_str());
    store.add_classes(std::move(classes));
  }
}

std::vector<std::string> list_modules(const std::string& path_str) {
  fs::path path(path_str);
  assert(fs::is_directory(path));

  auto end = fs::directory_iterator();
  std::vector<std::string> modules;
  for (fs::directory_iterator it(path) ; it != end ; ++it) {
    auto file = it->path();
    auto metadata = file;
    metadata += fs::path::preferred_separator;
    metadata += file.filename().string() + ".json";
    if (fs::is_directory(file) &&
      fs::is_regular_file(metadata) &&
      fs::exists(metadata)) {
      modules.emplace_back(file.filename().string());
    }
  }
  return modules;
}

} // namespace {

void Tool::add_standard_options(po::options_description& options) const {
  options.add_options()
    ("jars,j",
     po::value<std::string>()->value_name("foo.jar,bar.jar,...")->required(),
     "delimited list of system jars")
    ("apkdir,a",
     po::value<std::string>()->value_name("/tmp/redex_extracted_apk")->required(),
     "path of an apk dir obtained from redex.py -u")
    ("dexendir,d",
     po::value<std::string>()->value_name("/tmp/redex_dexen")->required(),
     "path of a dexen dir obtained from redex.py -u")
  ;
}

DexStoresVector Tool::init(
  const std::string& system_jar_paths,
  const std::string& apk_dir,
  const std::string& dexen_dir_str) {
  if (!fs::is_directory(fs::path(dexen_dir_str))) {
    throw std::invalid_argument("'" + dexen_dir_str + "' is not a directory");
  }

  // Load jars
  auto delim = boost::is_any_of(":,");
  std::vector<std::string> system_jars;
  boost::split(system_jars, system_jar_paths, delim);
  for (const auto& system_jar : system_jars) {
    std::cout << "Loading " << system_jar << std::endl;
    if (!load_jar_file(system_jar.c_str())) {
      throw std::runtime_error("Could not load system jar file '"+system_jar+"'");
    }
  }

  // Load dexen
  DexStore root_store("dex");
  DexStoresVector stores;

  // Load root dexen
  load_root_dexen(root_store, dexen_dir_str);
  stores.emplace_back(std::move(root_store));

  // Load module dexen
  for (auto module : list_modules(dexen_dir_str)) {
    fs::path metadata_path(dexen_dir_str);
    metadata_path += fs::path::preferred_separator;
    metadata_path += module;
    metadata_path += fs::path::preferred_separator;
    metadata_path += module + ".json";

    DexMetadata store_metadata;
    store_metadata.parse(metadata_path.string());
    DexStore store(store_metadata);
    load_store_dexen(store, store_metadata);
    stores.emplace_back(std::move(store));
  }

  // Initialize reachable classes
  std::cout << "Initializing reachable classes" << std::endl;
  Scope scope = build_class_scope(stores);
  Json::Value config;
  redex::ProguardConfiguration pg_config;
  // TODO: Need to get this from a redex .config file
  std::unordered_set<DexType*> no_optimizations_anno;
  init_reachable_classes(scope, config, pg_config, no_optimizations_anno);

  return stores;
}
