/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Util.h"

#include <cstdio>
#include <memory>
#include <string>

class FileHandle {
 public:
  explicit FileHandle(FILE* fh) : bytes_written_(0), seek_ref_(0), fh_(fh){};
  UNCOPYABLE(FileHandle);

  FILE* get() const { return fh_; }

  virtual ~FileHandle() {
    if (fh_ != nullptr) {
      fclose(fh_);
      fh_ = nullptr;
    }
  }

  FileHandle& operator=(FileHandle&& other) noexcept {
    bytes_written_ = other.bytes_written_;
    seek_ref_ = other.seek_ref_;
    fh_ = other.fh_;
    other.fh_ = nullptr;
    return *this;
  }

  FileHandle(FileHandle&& other) noexcept {
    bytes_written_ = other.bytes_written_;
    seek_ref_ = other.seek_ref_;
    fh_ = other.fh_;
    other.fh_ = nullptr;
  }

  size_t bytes_written() const { return bytes_written_; }
  void reset_bytes_written() { bytes_written_ = 0; }

  virtual size_t fwrite(const void* p, size_t size, size_t count);
  size_t fread(void* ptr, size_t size, size_t count);

  template <typename T>
  std::unique_ptr<T> read_object() {
    auto ret = std::unique_ptr<T>(new T);
    if (this->fread(ret.get(), sizeof(T), 1) != 1) {
      return std::unique_ptr<T>(nullptr);
    } else {
      return ret;
    }
  }

  bool feof();
  bool ferror();

  bool seek_set(long offset);
  bool seek_begin() { return seek_set(0); }
  bool seek_end();

  // Adjust the offset from which seek_set(N) is computed. Keeps oat-writing
  // code much cleaner by hiding the elf file .rodata offset from the oat code.
  void set_seek_reference_to_fpos();
  void set_seek_reference(long offset);

 protected:
  size_t fwrite_impl(const void* p, size_t size, size_t count);
  virtual void flush() {}
  size_t bytes_written_;

  // seek_set() operates relative to this point.
  long seek_ref_;

 private:
  FILE* fh_;
};

void write_word(FileHandle& fh, uint32_t value);
void write_short(FileHandle& fh, uint16_t value);
void write_str(FileHandle& fh, const std::string& str);
