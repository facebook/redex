/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Randomized malloc, for detecting memory-based non-determinisms in redex.
 * To use, build redex-all-malloc-dbg, and then run as follows (you'll probably
 * want all the args from a "real" redex run, but this is the idea):
 *
 *   MALLOC_SEED=seed1 ./redex-all-malloc-dbg --out out-seed1.apk in.apk
 *   MALLOC_SEED=seed2 ./redex-all-malloc-dbg --out out-seed2.apk in.apk
 *
 * If the two output APKs differ, it could be because of an indeterminism
 * caused by branching on pointer values (e.g. containers sorted by pointer
 * keys).
 *
 * Note that this is NOT an attempt to make a deterministic allocator. System
 * malloc is non deterministic (practically speaking), but it will often behave
 * very similarly, which can hide non determinisms caused by pointers. This
 * allocator is intended to make such non determinisms happen *every* time,
 * instead of only once in a while.
 */

#ifdef __APPLE__
#include <dlfcn.h>
#endif

#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <map>
#include <string>
#include <vector>

#include <thread>

#include "Debug.h"

namespace {

// The tinyest PRNG ever.
// http://www.woodmann.com/forum/showthread.php?3100-super-tiny-PRNG
class TinyPRNG {
 public:
  explicit TinyPRNG(const std::string& randSeed) { seed(randSeed); }

  uint32_t next_rand() {
    uint32_t result = 0;
    for (int i = 0; i < 32; i++) {
      // Advance the rand state.
      m_next_rand += (m_next_rand * m_next_rand) | 5;
      // pull out the high bit.
      result |= (m_next_rand >> 31) << i;
    }
    return result;
  }

  void seed(const std::string& randSeed) {
    constexpr auto state_size = sizeof(m_next_rand);
    char state[state_size] = {};
    std::string::size_type idx = 0;
    for (auto& e : randSeed) {
      state[idx % state_size] ^= e;
      idx++;
    }
    memcpy(&m_next_rand, state, state_size);
  }

 private:
  uint32_t m_next_rand = 0;
};

} // namespace

#ifdef __APPLE__
typedef void* (*MallocFn)(size_t);
static auto libc_malloc =
    reinterpret_cast<MallocFn>(dlsym(RTLD_NEXT, "malloc"));
typedef void* (*CallocFn)(size_t, size_t);
static auto libc_calloc =
    reinterpret_cast<CallocFn>(dlsym(RTLD_NEXT, "calloc"));
typedef void* (*MemalignFn)(size_t, size_t);
static auto libc_memalign =
    reinterpret_cast<MemalignFn>(dlsym(RTLD_NEXT, "memalign"));
typedef int (*PosixMemalignFn)(void**, size_t, size_t);
static auto libc_posix_memalign =
    reinterpret_cast<MemalignFn>(dlsym(RTLD_NEXT, "posix_memalign"));
#endif

#ifdef __linux__
extern "C" {
extern void* __libc_malloc(size_t size); // NOLINT(bugprone-reserved-identifier)
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern void* __libc_calloc(size_t nelem, size_t elsize);
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern void* __libc_memalign(size_t alignment, size_t size);
// This isn't found?
// extern int __posix_memalign(void** out, size_t alignment, size_t size);
}

static auto libc_malloc = __libc_malloc; // NOLINT(bugprone-reserved-identifier)
static auto libc_calloc = __libc_calloc; // NOLINT(bugprone-reserved-identifier)
// NOLINTNEXTLINE(bugprone-reserved-identifier)
static auto libc_memalign = __libc_memalign;
// static auto libc_posix_memalign = __posix_memalign;
#endif

namespace {

size_t next_power_of_two(size_t x) {
  // Turn off all but msb.
  while ((x & (x - 1)) != 0) {
    x &= x - 1;
  }
  return x << 1;
}

constexpr bool PRINT_SEED = false;

template <bool ENABLE_RAND>
class MallocDebug {
 public:
  explicit MallocDebug() : m_rand("wharblegarbl") {
    const char* seed_env = getenv("MALLOC_SEED");
    if (seed_env != nullptr) {
      if (PRINT_SEED) {
        printf("re-seeding with %s\n", seed_env);
      }
      m_rand.seed(seed_env);
    }
  }

  void* malloc(size_t size, bool randomize = true) noexcept {
    if (m_in_malloc) {
      return libc_malloc(size);
    }
    m_in_malloc = true;
    auto ret = malloc_impl<false>(
        randomize, size, m_blocks, [](size_t s) { return libc_malloc(s); });
    m_in_malloc = false;
    return ret;
  }

  void* calloc(size_t nelem, size_t elsize) noexcept {
    if (m_in_malloc) {
      return libc_calloc(nelem, elsize);
    }
    m_in_malloc = true;
    auto ret = malloc_impl<true>(false, nelem * elsize, m_blocks, [](size_t s) {
      return libc_malloc(s);
    });
    m_in_malloc = false;
    return ret;
  }

  void* memalign(size_t alignment,
                 size_t bytes,
                 bool randomize = true) noexcept {
    if (m_in_malloc) {
      return libc_memalign(alignment, bytes);
    }
    m_in_malloc = true;
    auto ret = malloc_impl<false>(
        randomize,
        bytes,
        m_aligned_blocks[alignment],
        [](size_t, size_t a, size_t b) { return libc_memalign(a, b); },
        alignment,
        bytes);
    m_in_malloc = false;
    return ret;
  }

  int posix_memalign(void** out,
                     size_t alignment,
                     size_t size,
                     bool randomize = true) noexcept {
    if (m_in_malloc) {
      *out = memalign(alignment, size);
      return 0;
    }
    m_in_malloc = true;
    auto ret = malloc_impl<false>(
        randomize,
        size,
        m_aligned_blocks[alignment],
        [](size_t, size_t a, size_t b) { return libc_memalign(a, b); },
        alignment,
        size);
    m_in_malloc = false;
    *out = ret;
    return 0;
  }

 private:
  struct Block {
    void* ptr;
    size_t size;
    Block(void* ptr, size_t size) : ptr(ptr), size(size) {}
    ~Block() { free(ptr); }

    Block(const Block&) = delete;
    Block(Block&& other) noexcept : ptr(other.release()), size(other.size) {}

    Block& operator=(const Block&) = delete;
    Block& operator=(Block&& rhs) noexcept {
      ptr = rhs.release();
      size = rhs.size;
      return *this;
    }

    void* release() {
      void* tmp = ptr;
      ptr = nullptr;
      return tmp;
    }
  };

  bool m_in_malloc = false;
  using BlockCache = std::map<size_t, std::vector<Block>>;
  BlockCache m_blocks;
  std::map<size_t, BlockCache> m_aligned_blocks;
  TinyPRNG m_rand;

  template <bool ZERO, typename Fn, typename... Args>
  void* malloc_impl(bool randomize,
                    size_t size,
                    BlockCache& blocks,
                    Fn fn,
                    Args... args) noexcept {
    size_t block_count = 8;

    // Size scheme:
    //   up to 1024, align by 4: this range has the highest alignment overhead
    //   up to 64k, align by 1024: that may amortize
    //   powers of 2 from here: not enough overlapping entries

    auto round_up = [](size_t n, size_t r) { return (n + r - 1) & ~(r - 1); };

    auto next_size = size <= 1024        ? round_up(size, 4)
                     : size <= 64 * 1024 ? round_up(size, 1024)
                                         : next_power_of_two(size);

    // For sizes >= 1M, reduce the block count.
    if (next_size >= 1024 * 1024) {
      block_count = 4;
    }

    auto it = blocks.find(next_size);
    if (it == blocks.end()) {
      it = blocks.emplace(next_size, std::vector<Block>()).first;
    }

    auto& vec = it->second;

    while (vec.size() < block_count) {
      auto ptr = fn(next_size, args...);
      vec.emplace_back(ptr, next_size);
    }

    auto idx = m_rand.next_rand() % block_count;
    auto block_size = vec[idx].size;
    void* block_ptr = vec[idx].release();
    vec.erase(vec.begin() + idx);

    always_assert(block_size >= size);

    if (ZERO) {
      // Zero out.
      memset(block_ptr, 0, size);
    } else if (ENABLE_RAND && randomize) {
      // Fill with garbage. Assume we have at least 4-byte alignment, and at
      // least 4-byte allocation (hence the std::max above).
      size_t len = (size + sizeof(uint32_t) - 1) / sizeof(uint32_t);
      for (size_t i = 0; i < len; ++i) {
        (reinterpret_cast<uint32_t*>(block_ptr))[i] = m_rand.next_rand();
      }
    }

    return block_ptr;
  }
};

thread_local MallocDebug<true> malloc_debug;
bool shutdown{false};

} // namespace

extern "C" {

void* malloc(size_t sz) {
  if (shutdown) {
    return libc_malloc(sz);
  }
  return malloc_debug.malloc(sz);
}

void* calloc(size_t nelem, size_t elsize) {
  if (shutdown) {
    return libc_calloc(nelem, elsize);
  }
  return malloc_debug.calloc(nelem, elsize);
}

void* memalign(size_t alignment, size_t bytes) {
  if (shutdown) {
    return libc_memalign(alignment, bytes);
  }
  return malloc_debug.memalign(alignment, bytes);
}

int posix_memalign(void** out, size_t alignment, size_t size) {
  if (shutdown) {
    *out = memalign(alignment, size);
    return 0;
  }
  return malloc_debug.posix_memalign(out, alignment, size);
}
} // extern "C"

namespace malloc_debug {

void set_shutdown() { shutdown = true; }

} // namespace malloc_debug
