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

#define PACKED __attribute__((packed))

#define CHECK(cond, ...)                        \
  do {                                          \
    auto cond_eval = (cond);                    \
    if (!cond_eval) {                           \
      char buf[512];                            \
      snprintf(buf, 511, "%s", "" __VA_ARGS__); \
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
