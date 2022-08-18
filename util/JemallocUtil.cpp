/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JemallocUtil.h"

#include <iostream>

#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>

#include "Debug.h"
#endif

namespace jemalloc_util {

#ifdef USE_JEMALLOC

namespace {

void set_profile_active(bool active) {
  int err =
      mallctl("prof.active", nullptr, nullptr, (void*)&active, sizeof(active));
  always_assert_log(err == 0, "mallctl failed with: %d", err);
}

} // namespace

void enable_profiling() { set_profile_active(true); }

void disable_profiling() { set_profile_active(false); }

void dump(const std::string& file_name) {
  auto* c_str = file_name.c_str();
  int err = mallctl("prof.dump", nullptr, nullptr, &c_str, sizeof(const char*));
  if (err != 0) {
    std::cerr << "mallctl failed with: " << err << std::endl;
  }
}

#else // !USE_JEMALLOC

void enable_profiling() {}

void disable_profiling() {}

void dump(const std::string&) {
  std::cerr << "Jemalloc dump unsupported" << std::endl;
}

#endif

} // namespace jemalloc_util
