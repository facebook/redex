/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexLoader.h"
#include "RedexTest.h"
#include <gtest/gtest.h>
#include <stdint.h>

class DexLoaderTest : public RedexTest {};

TEST_F(DexLoaderTest, dex_header_item_size) {
  // https://source.android.com/devices/tech/dalvik/dex-format#type-codes
  EXPECT_EQ(0x70, sizeof(dex_header));
}
