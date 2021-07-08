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

static constexpr bool kIsAsan = true;

#include <sanitizer/lsan_interface.h>

inline void lsan_do_leak_check() { __lsan_do_leak_check(); }
inline int lsan_do_recoverable_leak_check() {
  return __lsan_do_recoverable_leak_check();
}

#else

static constexpr bool kIsAsan = false;

inline void lsan_do_leak_check() {}
inline int lsan_do_recoverable_leak_check() { return 0; }

#endif // __has_feature(address_sanitizer)

} // namespace sanitizers
