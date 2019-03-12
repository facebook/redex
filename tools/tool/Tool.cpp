/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <iostream>

#include "DexLoader.h"
#include "DexUtil.h"
#include "JarLoader.h"
#include "ReachableClasses.h"
#include "Tool.h"

namespace fs = boost::filesystem;

namespace {

void load_store_dexen(DexStore& store,
                      const DexMetadata& store_metadata,
                      bool verbose,
                      bool balloon,
                      bool support_dex_v37 = false) {
  for (const auto& file_path : store_metadata.get_files()) {
    if (verbose) {
      std::cout << "Loading " << file_path << std::endl;
    }
    DexClasses classes =
        load_classes_from_dex(file_path.c_str(), balloon, support_dex_v37);
    store.add_classes(std::move(classes));
  }
}

DexMetadata parse_store_metadata(const fs::path& metadata_path) {
  DexMetadata metadata;
  std::ifstream file(metadata_path.string(), std::ios::in);
  std::string line;
  while (std::getline(file, line)) {
    std::vector<std::string> tokens;
    boost::split(tokens, line, boost::is_any_of(" "));
    if (tokens.size() > 1) {
      if (tokens[0] == ".id") {
        metadata.set_id(tokens[1]);
      } else if (tokens[0] == ".requires") {
        metadata.get_dependencies().emplace_back(tokens[1]);
      }
    }
  }
  return metadata;
}

std::vector<std::string> find_store_dexen(const fs::path& store_dir_path) {
  std::vector<std::string> dexen;
  auto end = fs::directory_iterator();
  for (fs::directory_iterator it(store_dir_path); it != end; ++it) {
    auto file = it->path();
    if (fs::is_regular_file(file) &&
        !file.extension().compare(std::string(".dex"))) {
      dexen.emplace_back(file.string());
    }
  }
  return dexen;
}

std::vector<DexMetadata> find_stores(const std::string& apk_dir_str,
                                     const std::string& dexen_dir_str) {
  fs::path apk_dir_path(apk_dir_str);
  fs::path dexen_dir_path(dexen_dir_str);
  auto end = fs::directory_iterator();
  std::vector<DexMetadata> metadatas;
  for (fs::directory_iterator it(dexen_dir_path); it != end; ++it) {
    // Look for metadata.txt for this store in the apk dir
    auto metadata_path = apk_dir_path;
    metadata_path += fs::path::preferred_separator;
    metadata_path += "assets";
    metadata_path += fs::path::preferred_separator;
    metadata_path += it->path().filename().string();
    metadata_path += fs::path::preferred_separator;
    metadata_path += "metadata.txt";
    if (fs::is_regular_file(metadata_path) && fs::exists(metadata_path)) {
      // Build metadata for store
      auto metadata = parse_store_metadata(metadata_path);
      metadata.set_files(find_store_dexen(it->path()));
      metadatas.emplace_back(metadata);
    }
  }
  return metadatas;
}

} // namespace

void Tool::add_standard_options(po::options_description& options) const {
  options.add_options()(
      "jars,j",
      po::value<std::string>()->value_name("foo.jar,bar.jar,...")->required(),
      "delimited list of system jars")(
      "apkdir,a",
      po::value<std::string>()
          ->value_name("/tmp/redex_extracted_apk")
          ->required(),
      "path of an apk dir obtained from redex.py -u")(
      "dexendir,d",
      po::value<std::string>()->value_name("/tmp/redex_dexen")->required(),
      "path of a dexen dir obtained from redex.py -u");
}

DexStoresVector Tool::init(const std::string& system_jar_paths,
                           const std::string& apk_dir_str,
                           const std::string& dexen_dir_str,
                           bool balloon,
                           bool support_dex_v37) {
  if (!fs::is_directory(fs::path(apk_dir_str))) {
    throw std::invalid_argument("'" + apk_dir_str + "' is not a directory");
  }
  if (!fs::is_directory(fs::path(dexen_dir_str))) {
    throw std::invalid_argument("'" + dexen_dir_str + "' is not a directory");
  }

  // Load jars
  if (system_jar_paths != "") {
    auto delim = boost::is_any_of(":,");
    std::vector<std::string> system_jars;
    boost::split(system_jars, system_jar_paths, delim);
    for (const auto& system_jar : system_jars) {
      if (m_verbose) {
        std::cout << "Loading " << system_jar << std::endl;
        if (!load_jar_file(system_jar.c_str())) {
          throw std::runtime_error("Could not load system jar file '" +
                                   system_jar + "'");
        }
      }
    }
  }

  // Load dexen
  DexStore root_store("dex");
  DexStoresVector stores;

  // Load root dexen
  load_root_dexen(
      root_store, dexen_dir_str, balloon, m_verbose, support_dex_v37);
  stores.emplace_back(std::move(root_store));

  // Load module dexen
  for (const auto& metadata : find_stores(apk_dir_str, dexen_dir_str)) {
    DexStore store(metadata);

    load_store_dexen(store, metadata, m_verbose, balloon, support_dex_v37);
    stores.emplace_back(std::move(store));
  }

  // Initialize reachable classes
  if (m_verbose) {
    std::cout << "Initializing reachable classes" << std::endl;
  }
  Scope scope = build_class_scope(stores);
  JsonWrapper config(Json::nullValue);
  // TODO: Need to get this from a redex .config file
  std::unordered_set<DexType*> no_optimizations_anno;
  init_reachable_classes(scope, config, no_optimizations_anno);

  return stores;
}
