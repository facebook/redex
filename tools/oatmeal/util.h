/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cassert>
#include <cstdio>
#include <string>
#include <utility>

#define CHECK(cond, ...)                        \
  do {                                          \
    auto cond_eval = (cond);                    \
    if (!cond_eval) {                           \
      char buf[512];                            \
      snprintf(buf, 511, "" __VA_ARGS__); \
      fprintf(stderr,                           \
              "%s:%d CHECK(%s) failed. %s\n",   \
              __FILE__,                         \
              __LINE__,                         \
              #cond,                            \
              buf);                             \
    }                                           \
    assert(cond_eval);                          \
  } while (0)

#define UNCOPYABLE(klass)       \
  klass(const klass&) = delete; \
  klass& operator=(const klass&) = delete;

#define MOVABLE(klass)      \
  klass(klass&&) = default; \
  klass& operator=(klass&&) = default;

template <uint32_t Width>
uint32_t align(uint32_t in) {
  return (in + (Width - 1)) & -Width;
}

template <uint32_t Width>
bool is_aligned(uint32_t in) {
  return (in & (Width - 1)) == 0;
}

template <typename T>
inline T nextPowerOfTwo(T in) {
  // Turn off all but msb.
  while ((in & (in - 1u)) != 0) {
    in &= in - 1u;
  }
  return in << 1u;
}

struct ConstBuffer {
  const char* ptr;
  size_t len;

  ConstBuffer slice(size_t new_begin) const { return slice(new_begin, len); }

  ConstBuffer truncate(size_t new_len) const {
    CHECK(new_len <= len);
    return ConstBuffer{ptr, new_len};
  }

  ConstBuffer slice(size_t new_begin, size_t new_end) const {
    CHECK(new_end <= len);
    CHECK(new_begin <= new_end);
    auto new_len = new_end - new_begin;
    return ConstBuffer{ptr + new_begin, new_len};
  }

  const char& operator[](size_t n) const {
    CHECK(n < len);
    return ptr[n];
  }
};

class FileHandle {
public:
  explicit FileHandle(FILE* fh) : fh_(fh), bytes_written_(0) {}
  UNCOPYABLE(FileHandle);

  FILE* get() const { return fh_; }

  virtual ~FileHandle() { if (fh_ != nullptr) { fclose(fh_); fh_ = nullptr; } }

  FileHandle& operator=(FileHandle&& other) {
    bytes_written_ = other.bytes_written_;
    fh_ = other.fh_;
    other.fh_ = nullptr;
    return *this;
  }

  FileHandle(FileHandle&& other) {
    bytes_written_ = other.bytes_written_;
    fh_ = other.fh_;
    other.fh_ = nullptr;
  }

  size_t bytes_written() const { return bytes_written_; }

  virtual size_t fwrite(const void* p, size_t size, size_t count);
  size_t fread(void* ptr, size_t size, size_t count);

  bool feof();
  bool ferror();

  bool seek_set(long offset);
  bool seek_begin() { return seek_set(0); }

protected:
  size_t fwrite_impl(const void* p, size_t size, size_t count);

private:
  FILE* fh_;
  size_t bytes_written_;
};

class Adler32 {
public:
  Adler32() = default;
  UNCOPYABLE(Adler32);
  MOVABLE(Adler32);

  void update(const void* _data, size_t len) {
    auto data = reinterpret_cast<const uint8_t*>(_data);
    for (size_t i = 0; i < len; i++) {
      a = (a + data[i]) % kModAdler;
      b = (b + a) % kModAdler;
    }
  }

  uint32_t get() const {
    return (b << 16) | a;
  }

  static uint32_t compute(ConstBuffer buf) {
    Adler32 adler;
    adler.update(buf.ptr, buf.len);
    return adler.get();
  }

private:
  static constexpr int kModAdler = 65521;

  uint32_t a = 1;
  uint32_t b = 0;
};

class ChecksummingFileHandle : public FileHandle {
public:
  ChecksummingFileHandle(FILE* fh, Adler32 cksum)
    : FileHandle(fh), cksum_(std::move(cksum)) {}

  ChecksummingFileHandle(FileHandle fh, Adler32 cksum)
    : FileHandle(std::move(fh)), cksum_(std::move(cksum)) {}

  size_t fwrite(const void* p, size_t size, size_t count) override;

  const Adler32& cksum() const { return cksum_; }

private:
  Adler32 cksum_;
};

void write_word(FileHandle& fh, uint32_t value);

void write_buf(FileHandle& fh, ConstBuffer buf);
void write_padding(FileHandle& fh, char byte, size_t num);

template <typename T>
void write_obj(FileHandle& fh, const T& obj) {
  write_buf(fh, ConstBuffer { reinterpret_cast<const char*>(&obj), sizeof(T) });
}

void write_str_and_null(FileHandle& fh, const std::string& str);
void write_str(FileHandle& fh, const std::string& str);
void stream_file(FileHandle& in, FileHandle& out);

size_t get_filesize(FileHandle& fh);

inline uint32_t read_uleb128(char** _ptr) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(*_ptr);
  uint32_t result = *(ptr++);

  if (result > 0x7f) {
    int cur = *(ptr++);
    result = (result & 0x7f) | ((cur & 0x7f) << 7);
    if (cur > 0x7f) {
      cur = *(ptr++);
      result |= (cur & 0x7f) << 14;
      if (cur > 0x7f) {
        cur = *(ptr++);
        result |= (cur & 0x7f) << 21;
        if (cur > 0x7f) {
          cur = *(ptr++);
          result |= cur << 28;
        }
      }
    }
  }
  *_ptr = reinterpret_cast<char*>(ptr);
  return result;
}
