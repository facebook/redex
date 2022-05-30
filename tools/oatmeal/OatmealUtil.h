/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexEncoding.h"
#include "DexOpcodeDefs.h"
#include "file-utils.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

//#define DEBUG_LOG
//#define PERF_LOG

#define PACK __attribute__((packed))

#ifdef PERF_LOG
#include <chrono>

#define START_TRACE() \
  auto trace_start = std::chrono::high_resolution_clock::now();

#define END_TRACE(TAG)                                        \
  using namespace std::chrono;                                \
  auto trace_end = std::chrono::high_resolution_clock::now(); \
  printf("TRACE[%s]: %llu ms\n",                              \
         TAG,                                                 \
         duration_cast<microseconds>(trace_end - trace_start).count());

#else
#define START_TRACE() \
  do {                \
  } while (0);

#define END_TRACE(TAG) \
  do {                 \
  } while (0);

#endif

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
inline T clz(T in) {
  static_assert(sizeof(T) == sizeof(uint64_t) || sizeof(T) == sizeof(uint32_t),
                "Unsupported size");
  return (sizeof(T) == sizeof(uint32_t)) ? __builtin_clz(in)
                                         : __builtin_clzll(in);
};

// This is a non-standard definition.
// roundUpToPowerOfTwo(x) = { nextPowerOfTwo(x) iff x < 2
//                            normalRoundUpToPowerOfTwo(x) iff x >= 2
// That is, rUp(0) = 1 and rUp(1) = 2, but rUp(2) = 2.
template <typename T>
inline T roundUpToPowerOfTwo(T in) {
  static_assert(std::is_unsigned<T>::value, "Only support unsigned types.");
  return (in < 2u) ? in + 1
                   : static_cast<T>(1u)
                         << (std::numeric_limits<T>::digits - clz(in - 1u));
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
      START_TRACE()
      write_buf(fh, ConstBuffer{begin, current});
      END_TRACE("buffer write")
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
    operator<<(const_cast<char*>(to_write));
  }

  void operator<<(const uint16_t* to_write) {
    const char* char_write = reinterpret_cast<const char*>(to_write);
    operator<<(char_write);
    operator<<(char_write + 1);
  }

  void operator<<(const uint16_t to_write) { operator<<(&to_write); }

  void print(size_t size) {
    size_t start = (current >= size) ? current - size : 0;
    for (size_t i = start; i < current; ++i) {
      printf("%02x%s", begin[i], (i == size - 1) ? "\r\n" : " ");
    }
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

bool is_vdex_file(ConstBuffer buf);

size_t get_filesize(FileHandle& fh);

std::string read_string(const uint8_t* dstr);

inline uint32_t read_uleb128(char** _ptr) {
  return read_uleb128(
      reinterpret_cast<const uint8_t**>(const_cast<const char**>(_ptr)));
}
