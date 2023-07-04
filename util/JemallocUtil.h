/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace jemalloc_util {

void enable_profiling();

void disable_profiling();

void dump(const std::string& file_name);

class ScopedProfiling final {
 public:
  explicit ScopedProfiling(bool enable) {
    if (enable) {
      fprintf(stderr, "Enabling memory profiling...\n");
      enable_profiling();
    }
  }

  ~ScopedProfiling() { disable_profiling(); }
};

std::string get_malloc_stats();
void some_malloc_stats(const std::function<void(const char*, uint64_t)>& fn);

} // namespace jemalloc_util
