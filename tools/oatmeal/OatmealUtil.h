/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "file-utils.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

template <typename T1, typename T2, typename L>
static void foreach_pair(const T1& t1, const T2& t2, const L& fn) {
  CHECK(t1.size() == t2.size());
  for (typename T1::size_type i = 0; i < t1.size(); i++) {
    fn(t1[i], t2[i]);
  }
}

template <uint32_t Width>
uint32_t align(uint32_t in) {
  return (in + (Width - 1)) & -Width;
}

inline uint32_t align(uint32_t width, uint32_t in) {
  return (in + (width - 1)) & -width;
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

template <typename T>
inline T countSetBits(T in) {
  // Turn off all but msb.
  if (in == 0) {
    return 0;
  }
  int count = 1;
  while ((in & (in - 1u)) != 0) {
    in &= in - 1u;
    count++;
  }
  return count;
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

void write_buf(FileHandle& fh, ConstBuffer buf);

struct WritableBuffer {
  FileHandle& fh;
  char* begin;
  size_t current;
  size_t max_size;

  WritableBuffer(FileHandle& fh_, char* begin_, size_t max_size_)
      : fh(fh_), begin(begin_), max_size(max_size_) {
    current = 0;
  }

  ~WritableBuffer() {
    if (current > 0) {
      write_buf(fh, ConstBuffer{begin, current});
    }
  }

  void operator<<(char* to_write) {
    if (current == max_size) {
      write_buf(fh, ConstBuffer{begin, current});
      current = 0;
    }
    begin[current] = *to_write;
    current++;
  }

  void operator<<(const char* to_write) {
    operator<<(const_cast<char *>(to_write));
  }

  void operator<<(const uint16_t* to_write) {
    const char* char_write = reinterpret_cast<const char*>(to_write);
    operator<<(char_write);
    operator<<(char_write + 1);
  }

  void operator<<(const uint16_t to_write) {
    operator<<(&to_write);
  }
};

void write_padding(FileHandle& fh, char byte, size_t num);

template <typename T>
void write_obj(FileHandle& fh, const T& obj) {
  write_buf(fh, ConstBuffer{reinterpret_cast<const char*>(&obj), sizeof(T)});
}

template <typename T>
void write_vec(FileHandle& fh, const std::vector<T>& obj) {
  write_buf(fh,
            ConstBuffer{reinterpret_cast<const char*>(obj.data()),
                        obj.size() * sizeof(T)});
}

void write_str_and_null(FileHandle& fh, const std::string& str);
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
