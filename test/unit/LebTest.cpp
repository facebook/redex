/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <array>

#include "DexEncoding.h"
#include "RedexTest.h"

class LebTest : public RedexTest {};

TEST_F(LebTest, Sleb128) {
  auto check = [](int32_t num, std::vector<uint8_t> bytes) {
    std::array<uint8_t, 5> storage = {0};
    size_t length = write_sleb128(storage.begin(), num) - storage.begin();

    EXPECT_EQ(length, bytes.size());
    for (unsigned i = 0; i < length; ++i) {
      EXPECT_EQ(storage[i], bytes[i]);
    }

    for (unsigned i = length; i < storage.size(); ++i) {
      EXPECT_EQ(storage[i], 0);
    }

    const uint8_t* storage_ptr = storage.begin();
    EXPECT_EQ(read_sleb128(&storage_ptr), num);
  };

  check((64 << 14), {0x80, 0x80, 0xC0, 0x00});
  check((64 << 14) - 1, {0xFF, 0xFF, 0x3F});
  check((64 << 7), {0x80, 0xC0, 0x00});
  check((64 << 7) - 1, {0xFF, 0x3F});
  check(64, {0xC0, 0x00});
  check(63, {0x3F});
  check(1, {0x01});
  check(0, {0x00});
  check(-1, {0x7F});
  check(-64, {0x40});
  check(-65, {0xBF, 0x7F});
  check(-(64 << 7), {0x80, 0x40});
  check(-(64 << 7) - 1, {0xFF, 0xBF, 0x7F});
  check(-(64 << 14), {0x80, 0x80, 0x40});
  check(-(64 << 14) - 1, {0xFF, 0xFF, 0xBF, 0x7F});
}
