/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "memory-accounter.h"
#include "OatmealUtil.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

// This class is a bit of a wart - the oat parsing code was initially written
// only for exploratory purposes, and MemoryAccounter exists so that we can
// make sure we've parsed and therefore understood all the bytes in an oat file.
// But now we're adding the ability to do oat parsing on device, for the
// purposes of madvise()ing regions of the oat file that correspond to various
// dex files, in which case we want to be able to run without forcing the caller
// to call MemoryAccounter::NewScope().
//
// Adding this stub class is easier than putting in null checks everywhere we
// call cur_ma().
class NilMemoryAccounterImpl : public MemoryAccounter {
 public:
  void print() override {}
  void memcpyAndMark(void* dest, const char* src, size_t count) override {
    memcpy(dest, src, count);
  }

  void markRangeConsumed(const char*, uint32_t) override {}
  void markBufferConsumed(ConstBuffer) override {}
  void addBuffer(ConstBuffer) override {}
};

class MultiBufferMemoryAccounter;

class MemoryAccounterImpl : public MemoryAccounter {
  friend class ::MemoryAccounterScope;
  friend class ::MultiBufferMemoryAccounter;

 public:
  UNCOPYABLE(MemoryAccounterImpl);
  MOVABLE(MemoryAccounterImpl);

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

  void addBuffer(ConstBuffer) override {
    CHECK(false, "Shouldn't do this here");
  }

  void memcpyAndMark(void* dest, const char* src, size_t count) override {
    CHECK(src >= buf_.ptr);
    uint32_t begin = src - buf_.ptr;
    uint32_t end = begin + count;

    markRangeImpl(begin, end);
    memcpy(dest, src, count);
  }

  void markRangeConsumed(const char* ptr, uint32_t count) override {
    CHECK(buf_.ptr <= ptr);

    uint32_t begin = ptr - buf_.ptr;
    uint32_t end = begin + count;
    markRangeImpl(begin, end);
  }

  void markBufferConsumed(ConstBuffer subBuffer) override {
    CHECK(subBuffer.ptr >= buf_.ptr);
    CHECK(subBuffer.ptr <= buf_.ptr + buf_.len);

    markRangeConsumed(subBuffer.ptr, subBuffer.len);
  }

  static MemoryAccounter* Cur() {
    if (accounter_stack_.size() == 0) {
      return &nil_accounter_;
    }
    return accounter_stack_.back().get();
  }

 private:
  struct Range {
    Range(size_t b, size_t e) : begin(b), end(e) {}
    uint32_t begin;
    uint32_t end;
  };

  ConstBuffer buf_;
  std::vector<Range> consumed_ranges_;

  static NilMemoryAccounterImpl nil_accounter_;
  static std::vector<std::unique_ptr<MemoryAccounter>> accounter_stack_;

  void markRangeImpl(uint32_t begin, uint32_t end) {
    CHECK(begin <= end);
    CHECK(end <= buf_.len);
    consumed_ranges_.emplace_back(begin, end);
  }
};

class MultiBufferMemoryAccounter : public MemoryAccounter {
 public:
  MultiBufferMemoryAccounter() = delete;
  MOVABLE(MultiBufferMemoryAccounter);
  UNCOPYABLE(MultiBufferMemoryAccounter);

  MultiBufferMemoryAccounter(ConstBuffer buf) { accounters_.emplace_back(buf); }

  void print() override;

  void memcpyAndMark(void* dest, const char* src, size_t count) override;

  void markRangeConsumed(const char* ptr, uint32_t count) override;
  void markBufferConsumed(ConstBuffer subBuffer) override;
  void addBuffer(ConstBuffer buf) override;

  ~MultiBufferMemoryAccounter() override = default;

 private:
  std::vector<MemoryAccounterImpl> accounters_;
};

void MultiBufferMemoryAccounter::memcpyAndMark(void* dest,
                                               const char* src,
                                               size_t count) {
  for (auto& a : accounters_) {
    auto ptr = a.buf_.ptr;
    char* dst_ptr = reinterpret_cast<char*>(dest);
    if (ptr <= dest && dest <= dst_ptr + count) {
      a.memcpyAndMark(dest, src, count);
      return;
    }
  }
  CHECK(false, "Can't find memory location");
}

void MultiBufferMemoryAccounter::markRangeConsumed(const char* ptr,
                                                   uint32_t count) {
  for (auto& a : accounters_) {
    auto base_ptr = a.buf_.ptr;
    if (base_ptr <= ptr && ptr + count <= base_ptr + a.buf_.len) {
      a.markRangeConsumed(ptr, count);
      return;
    }
  }
  CHECK(false, "Can't find memory location");
}

void MultiBufferMemoryAccounter::markBufferConsumed(ConstBuffer subBuffer) {
  for (auto& a : accounters_) {
    auto base_ptr = a.buf_.ptr;
    auto base_len = a.buf_.len;

    auto subbuf_ptr = subBuffer.ptr;
    auto subbuf_len = subBuffer.len;

    if (base_ptr <= subbuf_ptr &&
        subbuf_ptr + subbuf_len <= base_ptr + base_len) {
      a.markBufferConsumed(subBuffer);
      return;
    }
  }
  CHECK(false, "Can't find memory location");
}

void MultiBufferMemoryAccounter::print() {
  for (auto& a : accounters_) {
    a.print();
  }
}

void MultiBufferMemoryAccounter::addBuffer(ConstBuffer buf) {
  // Make sure this is no-ones sub-buffer in the currently accounted set.
  for (const auto& a : accounters_) {
    auto a_end = a.buf_.ptr + a.buf_.len;
    CHECK(a.buf_.ptr < buf.ptr || a.buf_.ptr > buf.ptr);
    CHECK(a_end < buf.ptr || a_end > buf.ptr + buf.len);
  }

  accounters_.emplace_back(buf);
}

NilMemoryAccounterImpl MemoryAccounterImpl::nil_accounter_;
std::vector<std::unique_ptr<MemoryAccounter>>
    MemoryAccounterImpl::accounter_stack_;
} // namespace

MemoryAccounter::~MemoryAccounter() = default;

MemoryAccounterScope MemoryAccounter::NewScope(ConstBuffer buf) {
  return MemoryAccounterScope(buf);
}

MemoryAccounter* MemoryAccounter::Cur() { return MemoryAccounterImpl::Cur(); }

MemoryAccounterScope::MemoryAccounterScope(ConstBuffer buf) {
  MemoryAccounterImpl::accounter_stack_.push_back(
      std::unique_ptr<MultiBufferMemoryAccounter>(
          new MultiBufferMemoryAccounter(buf)));
}

MemoryAccounterScope::~MemoryAccounterScope() {
  CHECK(MemoryAccounterImpl::accounter_stack_.size() > 0);
  MemoryAccounterImpl::accounter_stack_.pop_back();
}
