/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem.hpp>

#include <fstream>

namespace redex {

using namespace boost::filesystem;

struct TempDir {
  TempDir() = default;
  explicit TempDir(std::string path) : path(std::move(path)), released(false) {}
  TempDir(const TempDir& other) = delete;
  TempDir(TempDir&& other) noexcept
      : path(std::move(other.path)), released{other.released} {
    other.released = true;
  }

  ~TempDir() {
    if (!released) {
      remove_all(path);
    }
  }

  TempDir& operator=(const TempDir& rhs) = delete;
  TempDir& operator=(TempDir&& rhs) noexcept {
    path = std::move(rhs.path);
    released = rhs.released;
    rhs.released = true;
    return *this;
  }

  void release() { released = true; }

  std::string path;
  bool released{true};
};

inline TempDir make_tmp_dir(const char* boost_template_str) {
  auto path = temp_directory_path();
  path += path::preferred_separator;
  path += unique_path(boost_template_str);
  create_directories(path);
  return TempDir(path.string());
}

inline void copy_file(const std::string& from, const std::string& to) {
  std::ifstream src_stream(from, std::ios::binary);
  std::ofstream dest_stream(to, std::ios::binary);
  dest_stream << src_stream.rdbuf();
}

} // namespace redex
