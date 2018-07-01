/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Debug.h"

extern "C" {

// Weak symbols are not part of the language standard, but GCC and Clang support
// them.
#ifdef __GNUC__

// Weak symbol: the linker will not raise an error if it cannot resolve this.
// Instead, mallctl will just be a nullptr at runtime. This avoids making
// jemalloc a required build dependency.
int mallctl(const char* name,
            void* oldp,
            size_t* oldlenp,
            void* newp,
            size_t newlen) __attribute__((weak));

#endif

}

namespace {

void set_profile_active(bool active) {
#ifdef __GNUC__
  if (mallctl == nullptr) {
    return;
  }
  int err =
      mallctl("prof.active", nullptr, nullptr, (void*)&active, sizeof(active));
  always_assert_log(err == 0, "mallctl failed with: %d", err);
#else
  fprintf(stderr, "set_profile_active is a no-op without weak symbol support");
#endif
}

} // namespace

namespace jemalloc_util {

void enable_profiling() { set_profile_active(true); }

void disable_profiling() { set_profile_active(false); }

} // namespace jemalloc_util
