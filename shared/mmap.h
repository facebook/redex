/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Util.h"

#include <stddef.h>
#include <sys/types.h>

#include <map>
#include <string>

class MappedFile {
  UNCOPYABLE(MappedFile);

 public:
  static MappedFile* mmap_file(size_t byte_count,
                               int prot,
                               int flags,
                               int fd,
                               const char* filename,
                               std::string* error_msg);
  ~MappedFile();

  const std::string& name() const { return name_; }

  bool sync();

  uint8_t* begin() const { return begin_; }

  size_t size() const { return size_; }

  uint8_t* end() const { return begin() + size(); }

  bool has_address(const void* addr) const {
    return begin() <= addr && addr < end();
  }

 private:
  MappedFile(const std::string& name, uint8_t* begin, size_t size);

  const std::string name_;
  uint8_t* begin_; // Start of data.
  size_t size_; // Length of data.
};
