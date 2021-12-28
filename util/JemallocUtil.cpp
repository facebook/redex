/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include <iostream>

#include "Debug.h"

extern "C" {

using MallctlFn = int (*)(const char*, void*, size_t*, void*, size_t);

#if defined(__unix__) || defined(__APPLE__)
// We use dynamic lookup to avoid making jemalloc a required build dependency.
static auto mallctl =
    reinterpret_cast<MallctlFn>(dlsym(RTLD_DEFAULT, "mallctl"));
#else
MallctlFn mallctl = nullptr;
#endif
}

namespace {

void set_profile_active(bool active) {
  if (mallctl == nullptr) {
    return;
  }
  int err =
      mallctl("prof.active", nullptr, nullptr, (void*)&active, sizeof(active));
  always_assert_log(err == 0, "mallctl failed with: %d", err);
}

} // namespace

namespace jemalloc_util {

void enable_profiling() { set_profile_active(true); }

void disable_profiling() { set_profile_active(false); }

void dump(const std::string& file_name) {
  if (mallctl == nullptr) {
    return;
  }
  auto* c_str = file_name.c_str();
  int err = mallctl("prof.dump", nullptr, nullptr, &c_str, sizeof(const char*));
  if (err != 0) {
    std::cerr << "mallctl failed with: " << err << std::endl;
  }
}

} // namespace jemalloc_util
