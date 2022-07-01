/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <memory>

#include "../OatmealUtil.h"

TEST(OatmealUtil, empty) {
  EXPECT_EQ(1u, roundUpToPowerOfTwo(0u));
  EXPECT_EQ(2u, roundUpToPowerOfTwo(1u));
  EXPECT_EQ(2u, roundUpToPowerOfTwo(2u));
  EXPECT_EQ(4u, roundUpToPowerOfTwo(3u));
  EXPECT_EQ(4u, roundUpToPowerOfTwo(4u));
  EXPECT_EQ(8u, roundUpToPowerOfTwo(5u));
  EXPECT_EQ(8u, roundUpToPowerOfTwo(6u));
  EXPECT_EQ(8u, roundUpToPowerOfTwo(7u));
  EXPECT_EQ(8u, roundUpToPowerOfTwo(8u));
  EXPECT_EQ(16u, roundUpToPowerOfTwo(9u));

  EXPECT_EQ(1Lu, roundUpToPowerOfTwo(0Lu));
  EXPECT_EQ(2Lu, roundUpToPowerOfTwo(1Lu));
  EXPECT_EQ(2Lu, roundUpToPowerOfTwo(2Lu));
  EXPECT_EQ(4Lu, roundUpToPowerOfTwo(3Lu));
  EXPECT_EQ(4Lu, roundUpToPowerOfTwo(4Lu));
  EXPECT_EQ(8Lu, roundUpToPowerOfTwo(5Lu));
  EXPECT_EQ(8Lu, roundUpToPowerOfTwo(6Lu));
  EXPECT_EQ(8Lu, roundUpToPowerOfTwo(7Lu));
  EXPECT_EQ(8Lu, roundUpToPowerOfTwo(8Lu));
  EXPECT_EQ(16Lu, roundUpToPowerOfTwo(9Lu));

  EXPECT_EQ(0x100000000Lu, roundUpToPowerOfTwo(0xffffffffLu));
  EXPECT_EQ(0x100000000Lu, roundUpToPowerOfTwo(0x100000000Lu));
  EXPECT_EQ(0x200000000Lu, roundUpToPowerOfTwo(0x100000001Lu));
  EXPECT_EQ(0x200000000Lu, roundUpToPowerOfTwo(0x1ffffffffLu));
  EXPECT_EQ(0x200000000Lu, roundUpToPowerOfTwo(0x200000000Lu));
  EXPECT_EQ(0x400000000Lu, roundUpToPowerOfTwo(0x200000001Lu));
}
