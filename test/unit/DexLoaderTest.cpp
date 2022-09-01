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

TEST_F(DexLoaderTest, align_ptr) {
  uint8_t* const ALIGNED_BASE_PTR = (uint8_t*)(1 << 20);
  EXPECT_EQ(ALIGNED_BASE_PTR, align_ptr(ALIGNED_BASE_PTR + 0, 1));
  EXPECT_EQ(ALIGNED_BASE_PTR + 1, align_ptr(ALIGNED_BASE_PTR + 1, 1));
  EXPECT_EQ(ALIGNED_BASE_PTR + 2, align_ptr(ALIGNED_BASE_PTR + 2, 1));
  EXPECT_EQ(ALIGNED_BASE_PTR + 3, align_ptr(ALIGNED_BASE_PTR + 3, 1));

  EXPECT_EQ(ALIGNED_BASE_PTR + 0, align_ptr(ALIGNED_BASE_PTR + 0, 2));
  EXPECT_EQ(ALIGNED_BASE_PTR + 2, align_ptr(ALIGNED_BASE_PTR + 1, 2));
  EXPECT_EQ(ALIGNED_BASE_PTR + 2, align_ptr(ALIGNED_BASE_PTR + 2, 2));
  EXPECT_EQ(ALIGNED_BASE_PTR + 4, align_ptr(ALIGNED_BASE_PTR + 3, 2));

  EXPECT_EQ(ALIGNED_BASE_PTR + 0, align_ptr(ALIGNED_BASE_PTR + 0, 4));
  EXPECT_EQ(ALIGNED_BASE_PTR + 4, align_ptr(ALIGNED_BASE_PTR + 1, 4));
  EXPECT_EQ(ALIGNED_BASE_PTR + 4, align_ptr(ALIGNED_BASE_PTR + 2, 4));
  EXPECT_EQ(ALIGNED_BASE_PTR + 4, align_ptr(ALIGNED_BASE_PTR + 3, 4));
  EXPECT_EQ(ALIGNED_BASE_PTR + 4, align_ptr(ALIGNED_BASE_PTR + 4, 4));

  EXPECT_EQ(ALIGNED_BASE_PTR + 8, align_ptr(ALIGNED_BASE_PTR + 7, 8));
  EXPECT_EQ(ALIGNED_BASE_PTR + 8, align_ptr(ALIGNED_BASE_PTR + 8, 8));
  EXPECT_EQ(ALIGNED_BASE_PTR + 16, align_ptr(ALIGNED_BASE_PTR + 9, 8));
  EXPECT_EQ(ALIGNED_BASE_PTR + 16, align_ptr(ALIGNED_BASE_PTR + 15, 8));
  EXPECT_EQ(ALIGNED_BASE_PTR + 16, align_ptr(ALIGNED_BASE_PTR + 16, 8));
  EXPECT_EQ(ALIGNED_BASE_PTR + 24, align_ptr(ALIGNED_BASE_PTR + 17, 8));

  uint8_t* const UINTPTR_MAX_ALIGNED = (uint8_t*)((UINTPTR_MAX / 4) * 4);

  EXPECT_EQ(UINTPTR_MAX_ALIGNED, align_ptr(UINTPTR_MAX_ALIGNED - 3, 4));
  EXPECT_EQ(UINTPTR_MAX_ALIGNED, align_ptr(UINTPTR_MAX_ALIGNED - 2, 4));
  EXPECT_EQ(UINTPTR_MAX_ALIGNED, align_ptr(UINTPTR_MAX_ALIGNED - 1, 4));
  EXPECT_EQ(UINTPTR_MAX_ALIGNED, align_ptr(UINTPTR_MAX_ALIGNED - 0, 4));
}
