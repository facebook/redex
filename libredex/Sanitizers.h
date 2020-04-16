/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace sanitizers {

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#if __has_feature(address_sanitizer)

#include <sanitizer/lsan_interface.h>

void lsan_do_leak_check() { __lsan_do_leak_check(); }
int lsan_do_recoverable_leak_check() {
  return __lsan_do_recoverable_leak_check();
}

#else

void lsan_do_leak_check() {}
int lsan_do_recoverable_leak_check() { return 0; }

#endif // __has_feature(address_sanitizer)

} // namespace sanitizers
