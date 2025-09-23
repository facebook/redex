/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// NOTE: While this works and is mentioned in the googletest docs, it is not
//       the suggested way of doing something program-wide. However, creating
//       our own main function is a hassle, too.
//
// NOTE: It is unclear whether this works as expected with death tests.

#include <gtest/gtest.h>
#include <signal.h>

#include "DebugUtils.h"
#include "Macros.h"

namespace {

class RedexDebugEnvironment : public ::testing::Environment {
 public:
  ~RedexDebugEnvironment() override {}

  // Override this to define how to set up the environment.
  void SetUp() override {
    signal(SIGABRT, debug_backtrace_handler);
    signal(SIGINT, debug_backtrace_handler);
    signal(SIGSEGV, crash_backtrace_handler);
#if !IS_WINDOWS
    signal(SIGBUS, crash_backtrace_handler);
#endif
  }

  void TearDown() override {
    // We could try to remove our signal handlers, but it does not seem worth
    // it.
  }
};

testing::Environment* const s_redex_signal_env =
    testing::AddGlobalTestEnvironment(new RedexDebugEnvironment());

} // namespace
