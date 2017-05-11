/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "memory-accounter.h"
#include "util.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

class MemoryAccounterImpl : public MemoryAccounter {
 public:
  UNCOPYABLE(MemoryAccounterImpl);

  MemoryAccounterImpl(ConstBuffer buf) : buf_(buf) {
    // mark end to avoid special case in print.
    consumed_ranges_.emplace_back(buf_.len, buf_.len);
  }

  void print() override {

    std::sort(consumed_ranges_.begin(),
              consumed_ranges_.end(),
              [](const Range& a, const Range& b) { return a.begin < b.begin; });

    Range prev{0, 0};
    printf("Memory accounting:\n");
    if (consumed_ranges_.size() == 0) {
      printf("  no unconsumed memory found\n");
    }

    for (const auto& cur : consumed_ranges_) {
      if (prev.end < cur.begin) {
        printf("  unconsumed memory in range 0x%08x to 0x%08x\n",
               prev.end,
               cur.begin);
      }
      if (cur.begin < prev.end) {
        printf("  double consumed memory in range 0x%08x to 0x%08x\n",
               cur.begin,
               prev.end);
      }
      prev = cur;
    }
  }

  void memcpyAndMark(void* dest, const char* src, size_t count) override {
    CHECK(src >= buf_.ptr);
    uint32_t begin = src - buf_.ptr;
    uint32_t end = begin + count;
    CHECK(end <= buf_.len);

    consumed_ranges_.emplace_back(begin, end);
    memcpy(dest, src, count);
  }

  void markRangeConsumed(uint32_t begin, uint32_t count) override {
    uint32_t end = begin + count;
    CHECK(end <= buf_.len);
    consumed_ranges_.emplace_back(begin, end);
  }

  void markRangeConsumed(const char* ptr, uint32_t count) override {
    uint32_t begin = ptr - buf_.ptr;
    uint32_t end = begin + count;
    CHECK(end <= buf_.len);
    consumed_ranges_.emplace_back(begin, end);
  }

  void markBufferConsumed(ConstBuffer subBuffer) override {
    markRangeConsumed(subBuffer.ptr - buf_.ptr, subBuffer.len);
  }

 private:
  struct Range {
    Range(size_t b, size_t e) : begin(b), end(e) {}
    uint32_t begin;
    uint32_t end;
  };

  ConstBuffer buf_;
  std::vector<Range> consumed_ranges_;
};
}

MemoryAccounter::~MemoryAccounter() = default;

std::unique_ptr<MemoryAccounter> MemoryAccounter::New(ConstBuffer buf) {
  return std::unique_ptr<MemoryAccounter>(new MemoryAccounterImpl(buf));
}
