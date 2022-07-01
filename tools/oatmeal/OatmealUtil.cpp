/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OatmealUtil.h"
#include <cerrno>
#include <cstring>
#include <sys/stat.h>

void write_buf(FileHandle& fh, ConstBuffer buf) {
  CHECK(fh.fwrite(buf.ptr, sizeof(char), buf.len) == buf.len);
}

void write_str_and_null(FileHandle& fh, const std::string& str) {
  const auto len = str.size() + 1;
  CHECK(fh.fwrite(str.c_str(), sizeof(char), len) == len);
}

size_t get_filesize(FileHandle& fh) {
  auto fd = fileno(fh.get());
  struct stat dex_stat;
  CHECK(fstat(fd, &dex_stat) == 0, "fstat failed: %s", std::strerror(errno));
  return dex_stat.st_size;
}

void stream_file(FileHandle& in, FileHandle& out) {
  constexpr int kBufSize = 0x80000;
  std::unique_ptr<char[]> buf(new char[kBufSize]);

  do {
    auto num_read = in.fread(buf.get(), 1, kBufSize);
    CHECK(!in.ferror());
    if (num_read > 0) {
      write_buf(out, ConstBuffer{buf.get(), num_read});
    }
  } while (!in.feof());
}

void write_padding(FileHandle& fh, char byte, size_t num) {
  // This might be inefficient, but the most padding we ever do at
  // once is 4k, so this shouldn't be too bad.
  for (size_t i = 0; i < num; i++) {
    CHECK(fh.fwrite(&byte, sizeof(char), 1) == sizeof(char));
  }
}

std::string read_string(const uint8_t* dstr) {
  // int utfsize = read_uleb128(&dstr);
  read_uleb128(&dstr);
  std::string rt((const char*)dstr);
  return rt;
}
