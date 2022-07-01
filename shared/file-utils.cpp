/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "file-utils.h"

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

bool FileHandle::feof() { return ::feof(fh_) != 0; }

bool FileHandle::ferror() { return ::ferror(fh_) != 0; }

bool FileHandle::seek_set(long offset) {
  flush();
  return ::fseek(fh_, offset + seek_ref_, SEEK_SET) == 0;
}

bool FileHandle::seek_end() {
  flush();
  return ::fseek(fh_, 0, SEEK_END) == 0;
}

void FileHandle::set_seek_reference_to_fpos() {
  set_seek_reference(::ftell(fh_));
}

void FileHandle::set_seek_reference(long offset) { seek_ref_ = offset; }

void write_word(FileHandle& fh, uint32_t value) {
  auto bytes_written = fh.fwrite(&value, sizeof(value), 1) * sizeof(value);
  if (bytes_written != sizeof(value)) {
    fprintf(stderr, "fwrite wrote %zd, not %zd\n", bytes_written,
            sizeof(value));
  }
  CHECK(bytes_written == sizeof(value));
}

void write_short(FileHandle& fh, uint16_t value) {
  auto bytes_written = fh.fwrite(&value, sizeof(value), 1) * sizeof(value);
  if (bytes_written != sizeof(value)) {
    fprintf(stderr, "fwrite wrote %zd, not %zd\n", bytes_written,
            sizeof(value));
  }
  CHECK(bytes_written == sizeof(value));
}

void write_str(FileHandle& fh, const std::string& str) {
  const auto len = str.size();
  CHECK(fh.fwrite(str.c_str(), sizeof(char), len) == len);
}
