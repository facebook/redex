/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

#include "Debug.h"

inline void open_or_die(const std::string& filename, std::ofstream* os) {
  os->open(filename);
  if (!os->is_open()) {
    std::cerr << "Unable to open: " << filename << std::endl;
    exit(EXIT_FAILURE);
  }
}

inline void write_string_to_file(const std::string& filename,
                                 const std::string& contents) {
  std::ofstream out(filename, std::ofstream::binary);
  out << contents;
}

// Given a set of relative files from the zip root, delete from the unpacked dir
// asserting that all deletions were successful.
inline size_t delete_files_relative(
    const std::string& apk_dir,
    const std::unordered_set<std::string>& relative_file_paths) {
  size_t actually_deleted = 0;
  for (const auto& f : relative_file_paths) {
    std::string full_path = apk_dir + "/" + f;
    if (exists(boost::filesystem::path(full_path))) {
      auto remove_res = std::remove(full_path.c_str());
      always_assert(remove_res == 0);
      actually_deleted++;
    }
  }
  return actually_deleted;
}

inline size_t delete_files_absolute(
    const std::unordered_set<std::string>& absolute_file_paths) {
  size_t actually_deleted = 0;
  for (const auto& path : absolute_file_paths) {
    if (exists(boost::filesystem::path(path))) {
      auto remove_res = std::remove(path.c_str());
      always_assert(remove_res == 0);
      actually_deleted++;
    }
  }
  return actually_deleted;
}
