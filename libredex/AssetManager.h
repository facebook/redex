/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem.hpp>
#include <memory>
#include <string>
#include <vector>

#include "RedexResources.h"

class AssetManager {
 public:
  explicit AssetManager(std::string dir) {
    if (has_bundle_config(dir)) {
      m_base_dir = (boost::filesystem::path(dir) / "base").string();
    } else {
      m_base_dir = dir;
    }
  }

  virtual ~AssetManager() {
    for (auto& fd : m_files) {
      if (*fd != nullptr) {
        fclose(*fd);
        fd = nullptr;
      }
    }
  }

  bool has_secondary_dex_dir();
  std::shared_ptr<FILE*> new_asset_file(
      const char* filename,
      const char* dir_path = "/assets/secondary-program-dex-jars/",
      bool new_dir = false);

 private:
  std::vector<std::shared_ptr<FILE*>> m_files;
  std::string m_base_dir;
};
