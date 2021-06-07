/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AssetManager.h"

#include <boost/filesystem.hpp>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

void create_directories_if_not_exists(std::string& dir) {
  if (!boost::filesystem::exists(dir.c_str())) {
    boost::filesystem::create_directories(dir.c_str());
  }
}

void check_directory(std::string& dir) {
  if (dir.empty()) {
    std::cerr << "error: directory not provided" << std::endl;
    exit(EXIT_FAILURE);
  }
  if (!boost::filesystem::is_directory(dir.c_str())) {
    std::cerr << "error: not a writable directory: " << dir << std::endl;
    exit(EXIT_FAILURE);
  }
}

} // namespace

bool AssetManager::has_asset_dir() {
  if (m_base_dir.empty()) {
    return false;
  }
  check_directory(m_base_dir);
  std::ostringstream path;
  path << m_base_dir << "/assets/secondary-program-dex-jars/";
  std::string assets_dir = path.str();
  return boost::filesystem::is_directory(assets_dir.c_str());
}

std::shared_ptr<FILE*> AssetManager::new_asset_file(const char* filename,
                                                    const char* dir_path,
                                                    bool new_dir) {
  check_directory(m_base_dir);
  std::ostringstream path;
  path << m_base_dir << dir_path;
  std::string assets_dir = path.str();
  if (new_dir) {
    create_directories_if_not_exists(assets_dir);
  } else {
    check_directory(assets_dir);
  }
  path << filename;

  FILE* fd = fopen(path.str().c_str(), "w");
  if (fd != nullptr) {
    m_files.emplace_back(std::make_shared<FILE*>(fd));
    return m_files.back();
  } else {
    throw new std::runtime_error("Error creating new asset file");
  }
}
