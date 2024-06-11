/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ReachableServiceLoaderVerifyImpl.h"

TEST_F(PreVerify, ApkReachableServiceLoaderTest) {
  verify_exception_handlers_kept(classes);
}

TEST_F(PostVerify, ApkReachableServiceLoaderTest) {
  verify_exception_handlers_kept(classes);
}
