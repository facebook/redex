/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Debug.h"
#include "RedexTest.h"

class DebugTest : public RedexTest {};

TEST_F(DebugTest, slow_invariants_on_for_gtest) {
  EXPECT_TRUE(slow_invariants_debug);
}
