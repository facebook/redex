/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <gtest/gtest.h>

#include "DexAnnotation.h"

void testcase(uint64_t value, std::initializer_list<uint8_t> t) {
  uint8_t buf[16] = {0};
  uint8_t* bufp = buf;
  int size = (t.end() - t.begin()) - 1;
  type_encoder_signext(bufp, DEVT_INT, value);
  EXPECT_EQ(bufp - buf, size + 2);
  EXPECT_EQ(buf[0], ((size << 5) | DEVT_INT));
  EXPECT_EQ(true, std::equal(t.begin(), t.end(), buf + 1));
}

TEST(EvWriteTest, empty) {
  uint8_t buf[16] = {0};
  uint8_t* bufp = buf;

  testcase(0xffffffffffffff37, {0x37, 0xff});
  testcase(0x37, {0x37});
  testcase(0x0, {0x00});
  testcase(-1, {0xff});
  testcase(0xffffffffffffffff, {0xff});
  testcase(0xdead, {0xad, 0xde, 0x00});
  testcase(0xff, {0xff, 0x00});
  testcase(0x80, {0x80, 0x00});
  testcase(0xffffffffffffff00, {0x00, 0xff});
}
