/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// WARNING: Only include this file once. Otherwise duplicate symbols errors
//          may occur.

// Some defaults for sanitizers. Can be overridden with ASAN_OPTIONS.
// NOLINTNEXTLINE(misc-definitions-in-headers)
const char* kAsanDefaultOptions =
    "abort_on_error=1" // Use abort instead of exit, will get stack
                       // traces for things like ubsan.
    ":"
    "check_initialization_order=1"
    ":"
    "detect_invalid_pointer_pairs=1"
    ":"
    "detect_leaks=0"
    ":"
    "detect_stack_use_after_return=1"
    ":"
    "print_scariness=1"
    ":"
    "print_suppressions=0"
    ":"
    "strict_init_order=1"
    ":"
    "detect_container_overflow=1"
    ":"
    "detect_stack_use_after_scope=1";

#if defined(__clang__)
#define NO_SANITIZE \
  __attribute__((__no_sanitize__("address", "undefined", "thread")))
#define VISIBLE \
  __attribute__((__visibility__("default"))) __attribute__((__used__))
#else
#define NO_SANITIZE
#define VISIBLE
#endif

extern "C" NO_SANITIZE VISIBLE __attribute__((__weak__)) const char*
// NOLINTNEXTLINE(misc-definitions-in-headers)
__asan_default_options() {
  return kAsanDefaultOptions;
}
