/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexMappedFile.h"

#include <boost/iostreams/device/mapped_file.hpp>
#include <fstream>

#include "Debug.h"

RedexMappedFile::RedexMappedFile(
    std::unique_ptr<boost::iostreams::mapped_file> file,
    std::string filename,
    bool read_only) noexcept
    : file(std::move(file)),
      filename(std::move(filename)),
      read_only(read_only) {}

RedexMappedFile::~RedexMappedFile() {}

RedexMappedFile::RedexMappedFile(RedexMappedFile&& other) noexcept {
  file = std::move(other.file);
  filename = std::move(other.filename);
  read_only = other.read_only;
}

RedexMappedFile& RedexMappedFile::operator=(RedexMappedFile&& rhs) noexcept {
  file = std::move(rhs.file);
  filename = std::move(rhs.filename);
  read_only = rhs.read_only;
  return *this;
}

RedexMappedFile RedexMappedFile::open(std::string path, bool read_only) {
  auto map = std::make_unique<boost::iostreams::mapped_file>();
  std::ios_base::openmode mode =
      (std::ios_base::openmode)(std::ios_base::in |
                                (read_only ? 0 : std::ios_base::out));
  map->open(path, mode);
  if (!map->is_open()) {
    throw std::runtime_error(std::string("Could not map ") + path);
  }

  return RedexMappedFile(std::move(map), std::move(path), read_only);
}

const char* RedexMappedFile::const_data() const { return file->const_data(); }
char* RedexMappedFile::data() const {
  redex_assert(!read_only);
  return file->data();
}
size_t RedexMappedFile::size() const { return file->size(); }
