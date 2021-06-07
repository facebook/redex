/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

class AssetManager {
 public:
  explicit AssetManager(std::string apk_dir) : m_apk_dir(std::move(apk_dir)) {}

  virtual ~AssetManager() {
    for (auto& fd : m_files) {
      if (*fd != nullptr) {
        fclose(*fd);
        fd = nullptr;
      }
    }
  }

  bool has_asset_dir();
  std::shared_ptr<FILE*> new_asset_file(
      const char* filename,
      const char* dir_path = "/assets/secondary-program-dex-jars/",
      bool new_dir = false);

 private:
  std::vector<std::shared_ptr<FILE*>> m_files;
  std::string m_apk_dir;
};
