/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
  EXPECT_EQ((uint8_t*)0, align_ptr((uint8_t*)0, 1));
  EXPECT_EQ((uint8_t*)1, align_ptr((uint8_t*)1, 1));
  EXPECT_EQ((uint8_t*)2, align_ptr((uint8_t*)2, 1));
  EXPECT_EQ((uint8_t*)3, align_ptr((uint8_t*)3, 1));

  EXPECT_EQ((uint8_t*)0, align_ptr((uint8_t*)0, 2));
  EXPECT_EQ((uint8_t*)2, align_ptr((uint8_t*)1, 2));
  EXPECT_EQ((uint8_t*)2, align_ptr((uint8_t*)2, 2));
  EXPECT_EQ((uint8_t*)4, align_ptr((uint8_t*)3, 2));

  EXPECT_EQ((uint8_t*)0, align_ptr((uint8_t*)0, 4));
  EXPECT_EQ((uint8_t*)4, align_ptr((uint8_t*)1, 4));
  EXPECT_EQ((uint8_t*)4, align_ptr((uint8_t*)2, 4));
  EXPECT_EQ((uint8_t*)4, align_ptr((uint8_t*)3, 4));
  EXPECT_EQ((uint8_t*)4, align_ptr((uint8_t*)4, 4));

  EXPECT_EQ((uint8_t*)8, align_ptr((uint8_t*)7, 8));
  EXPECT_EQ((uint8_t*)8, align_ptr((uint8_t*)8, 8));
  EXPECT_EQ((uint8_t*)16, align_ptr((uint8_t*)9, 8));
  EXPECT_EQ((uint8_t*)16, align_ptr((uint8_t*)15, 8));
  EXPECT_EQ((uint8_t*)16, align_ptr((uint8_t*)16, 8));
  EXPECT_EQ((uint8_t*)24, align_ptr((uint8_t*)17, 8));

  uint8_t* const UINTPTR_MAX_ALIGNED = (uint8_t*)((UINTPTR_MAX / 4) * 4);

  EXPECT_EQ(UINTPTR_MAX_ALIGNED, align_ptr(UINTPTR_MAX_ALIGNED - 3, 4));
  EXPECT_EQ(UINTPTR_MAX_ALIGNED, align_ptr(UINTPTR_MAX_ALIGNED - 2, 4));
  EXPECT_EQ(UINTPTR_MAX_ALIGNED, align_ptr(UINTPTR_MAX_ALIGNED - 1, 4));
  EXPECT_EQ(UINTPTR_MAX_ALIGNED, align_ptr(UINTPTR_MAX_ALIGNED - 0, 4));

  // Not sure if it's safe to assume that UINTPTR_MAX will always be
  // identical to (uintXX_t)(-1) for some integer width XX, and will always
  // overflow to 0 whenever aligning some address beyond UINTPTR_MAX_ALIGNED.
  // But on systems where that is indeed the case, we can test for it:
  if ((uint8_t*)0 == (uint8_t*)(1U + UINTPTR_MAX)) {
    for (size_t i = 0; i < (UINTPTR_MAX % 4); ++i) {
      EXPECT_EQ((uint8_t*)0, align_ptr((uint8_t*)(UINTPTR_MAX - i), 4));
    }
  }
}
