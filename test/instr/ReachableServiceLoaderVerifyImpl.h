/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gtest/gtest.h>

#include "verify/VerifyUtil.h"

inline void verify_exception_handlers_kept(const DexClasses& classes) {
  const char* const INTERFACE_NAME =
      "Lkotlinx/coroutines/CoroutineExceptionHandler;";
  const char* const IMPLEMENTATION_NAME =
      "Lkotlinx/coroutines/android/AndroidExceptionPreHandler;";

  auto iface = find_class_named(classes, INTERFACE_NAME);
  EXPECT_NE(iface, nullptr) << "Did not find class " << INTERFACE_NAME;
  auto cls = find_class_named(classes, IMPLEMENTATION_NAME);
  EXPECT_NE(cls, nullptr) << "Did not find class " << IMPLEMENTATION_NAME;
}
