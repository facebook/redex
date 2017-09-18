/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>
#include <map>
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

}

#ifdef __APPLE__
typedef void* (*MallocFn)(size_t);
static auto libc_malloc = reinterpret_cast<MallocFn>(dlsym(RTLD_NEXT, "malloc"));
#endif

#ifdef __linux__
extern "C" {
extern void *__libc_malloc(size_t size);
}

static auto libc_malloc = __libc_malloc;
#endif

size_t next_power_of_two(size_t x) {
  // Turn off all but msb.
  while ((x & (x - 1)) != 0) {
    x &= x - 1;
  }
  return x << 1;
}

namespace {
class MallocDebug {
 public:
  explicit MallocDebug() : m_rand("wharblegarbl") {
    const char* seed_env = getenv("MALLOC_SEED");
    if (seed_env != nullptr) {
      printf("re-seeding with %s\n", seed_env);
      m_rand.seed(seed_env);
    }
  }

  void* malloc(size_t size) noexcept {
    if (m_in_malloc) {
      return libc_malloc(size);
    }
    m_in_malloc = true;
    auto ret = malloc_impl(size);
    m_in_malloc = false;
    return ret;
  }

 private:

  struct Block {
    void* ptr;
    size_t size;
  };

  bool m_in_malloc = false;
  std::map<size_t, std::vector<Block>> m_blocks;
  TinyPRNG m_rand;

  void* malloc_impl(size_t size) noexcept {
    constexpr int block_count = 8;

    auto next_size = next_power_of_two(size);

    auto it = m_blocks.find(next_size);
    if (it == m_blocks.end()) {
      it = m_blocks.emplace(next_size, std::vector<Block>()).first;
    }

    auto& vec = it->second;

    while (vec.size() < block_count) {
      auto ptr = libc_malloc(next_size);
      vec.push_back(Block { ptr, next_size });
    }

    auto idx = m_rand.next_rand() % block_count;
    auto block = vec[idx];
    vec.erase(vec.begin() + idx);

    always_assert(block.size >= size);
    return block.ptr;
  }

};

thread_local MallocDebug malloc_debug;

}

extern "C" {

void* malloc(size_t sz) { return malloc_debug.malloc(sz); }

}
