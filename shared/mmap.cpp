/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "mmap.h"

#include <inttypes.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <memory>
#include <sstream>

MappedFile* MappedFile::mmap_file(size_t byte_count,
                                  int prot,
                                  int flags,
                                  int fd,
                                  const char* filename,
                                  std::string* error_msg) {
  off_t offset = 0;
  CHECK(prot != 0);
  CHECK((flags & (MAP_SHARED | MAP_PRIVATE)) != 0);
  CHECK((flags & MAP_FIXED) == 0);

  if (byte_count == 0) {
    return new MappedFile(filename, nullptr, 0);
  }

  uint8_t* actual = reinterpret_cast<uint8_t*>(
      mmap(nullptr /* expected ptr */, byte_count, prot, flags, fd, offset));

  if (actual == MAP_FAILED) {
    if (error_msg != nullptr) {

      fprintf(stderr, "mmap(%zu, %jd, 0x%x, 0x%x, %d) of file '%s' failed\n",
              byte_count, (intmax_t)offset, prot, flags, fd, filename);
    }
    return nullptr;
  }

  return new MappedFile(filename, actual, byte_count);
}

MappedFile::~MappedFile() {
  if (begin_ == nullptr && size_ == 0) {
    return;
  }

  int result = munmap(begin_, size_);
  if (result == -1) {
    fprintf(stderr, "munmap failed\n");
  }
}

MappedFile::MappedFile(const std::string& _name, uint8_t* _begin, size_t _size)
    : name_(_name), begin_(_begin), size_(_size) {
  if (size_ == 0) {
    CHECK(begin_ == nullptr);
  } else {
    CHECK(begin_ != nullptr);
  }
}

bool MappedFile::sync() { return msync(begin(), size(), MS_SYNC) == 0; }
