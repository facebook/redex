/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

namespace boost {
namespace iostreams {
class mapped_file;
} // namespace iostreams
} // namespace boost

struct RedexMappedFile {
  std::unique_ptr<boost::iostreams::mapped_file> file;
  std::string filename;
  bool read_only;

  RedexMappedFile(std::unique_ptr<boost::iostreams::mapped_file> in_file,
                  std::string in_filename,
                  bool read_only) noexcept;
  ~RedexMappedFile();

  RedexMappedFile(RedexMappedFile&& other) noexcept;
  RedexMappedFile& operator=(RedexMappedFile&& rhs) noexcept;

  RedexMappedFile(const RedexMappedFile&) = delete;
  RedexMappedFile& operator=(const RedexMappedFile&) = delete;

  static RedexMappedFile open(std::string path, bool read_only = true);

  const char* const_data() const;
  char* data() const;
  size_t size() const;
};
