/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "util.h"
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

size_t FileHandle::fwrite_impl(const void* p, size_t size, size_t count) {
  auto ret = ::fwrite(p, size, count, fh_);
  return ret;
}

size_t FileHandle::fwrite(const void* p, size_t size, size_t count) {
  auto ret = fwrite_impl(p, size, count);
  bytes_written_ += ret * size;
  return ret;
}

size_t FileHandle::fread(void* p, size_t size, size_t count) {
  return ::fread(p, size, count, fh_);
}

bool FileHandle::feof() {
  return ::feof(fh_) != 0;
}

bool FileHandle::ferror() {
  return ::ferror(fh_) != 0;
}

bool FileHandle::seek_set(long offset) {
  flush();
  return ::fseek(fh_, offset, SEEK_SET) == 0;
}

size_t ChecksummingFileHandle::fwrite(const void* p, size_t size, size_t count) {
  const auto bytes = size * count;

  if (buf_pos_ + bytes >= kBufSize) {
    flush();
  }

  if (bytes >= kBufSize) {
    // we've already flushed here.
    auto ret = flush_write(reinterpret_cast<const char*>(p), size, count);
    bytes_written_ += bytes;
    return ret;
  }

  CHECK(buf_pos_ + bytes < kBufSize);
  memcpy(&buffer_[buf_pos_], p, bytes);
  buf_pos_ += bytes;
  bytes_written_ += bytes;
  return count;
}

void ChecksummingFileHandle::flush() {
  if (buf_pos_ == 0) {
    return;
  }
  CHECK(flush_write(buffer_.get(), 1, buf_pos_) == buf_pos_);
  buf_pos_ = 0;
}

size_t ChecksummingFileHandle::flush_write(const char* p, size_t size, size_t count) {
  cksum_.update(p, size * count);
  return fwrite_impl(p, size, count);
}

void write_word(FileHandle& fh, uint32_t value) {
  auto bytes_written = fh.fwrite(&value, sizeof(value), 1) * sizeof(value);
  if (bytes_written != sizeof(value)) {
    fprintf(stderr, "fwrite wrote %lu, not %lu\n", bytes_written, sizeof(value));
  }
  CHECK(bytes_written == sizeof(value));
}

void write_buf(FileHandle& fh, ConstBuffer buf) {
  CHECK(fh.fwrite(buf.ptr, sizeof(char), buf.len) == buf.len);
}

void write_str_and_null(FileHandle& fh, const std::string& str) {
  const auto len = str.size() + 1;
  CHECK(fh.fwrite(str.c_str(), sizeof(char), len) == len);
}

void write_str(FileHandle& fh, const std::string& str) {
  const auto len = str.size();
  CHECK(fh.fwrite(str.c_str(), sizeof(char), len) == len);
}

size_t get_filesize(FileHandle& fh) {
  auto fd = fileno(fh.get());
  struct stat dex_stat;
  CHECK(fstat(fd, &dex_stat) == 0,
        "fstat failed: %s", std::strerror(errno));
  return dex_stat.st_size;
}

void stream_file(FileHandle& in, FileHandle& out) {
  constexpr int kBufSize = 0x80000;
  std::unique_ptr<char[]> buf(new char[kBufSize]);

  do {
    auto num_read = in.fread(buf.get(), 1, kBufSize);
    CHECK(!in.ferror());
    if (num_read > 0) {
      write_buf(out, ConstBuffer { buf.get(), num_read });
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
