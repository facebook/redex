/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>

#include "DexAnnotation.h"

TEST(FpEvTest, empty) {
#define test(x, ans...)                                                   \
  do {                                                                    \
    uint8_t res[] = {ans};                                                \
    uint8_t buf[9];                                                       \
                                                                          \
    uint64_t val = x;                                                     \
    std::vector<uint8_t> vbuf;                                            \
    type_encoder_fp(vbuf, DEVT_FLOAT, val);                               \
    for (size_t i = 0; i < 9 && i < vbuf.size(); i++)                     \
      buf[i] = vbuf[i];                                                   \
    [[maybe_unused]] uint8_t* bufp = buf + vbuf.size();                   \
                                                                          \
    EXPECT_EQ(buf[0], DEVT_FLOAT | (sizeof(res) - 1) << 5);               \
    EXPECT_EQ(memcmp(res, &buf[1], sizeof(res)), 0);                      \
                                                                          \
    const uint8_t* cbufp = buf;                                           \
    uint8_t evhdr = *cbufp++;                                             \
    uint8_t evarg = DEVT_HDR_ARG(evhdr);                                  \
    uint64_t nval = read_evarg(cbufp, evarg, false) << ((3 - evarg) * 8); \
                                                                          \
    EXPECT_EQ(val, nval);                                                 \
  } while (0);

  test(0x00, 0x00);
  test(0x01, 0x01, 0x00, 0x00, 0x00);
  test(0x80, 0x80, 0x00, 0x00, 0x00);
  test(0xff, 0xff, 0x00, 0x00, 0x00);
  test(0x0100, 0x01, 0x00, 0x00);
  test(0x0101, 0x01, 0x01, 0x00, 0x00);
  test(0x8000, 0x80, 0x00, 0x00);
  test(0x8001, 0x01, 0x80, 0x00, 0x00);
  test(0x010000, 0x01, 0x00);
  test(0x010001, 0x01, 0x00, 0x01, 0x00);
  test(0x010100, 0x01, 0x01, 0x00);
  test(0x010101, 0x01, 0x01, 0x01, 0x00);
  test(0x01000000, 0x01);
  test(0x01001000, 0x10, 0x00, 0x01);
  test(0x01000002, 0x02, 0x00, 0x00, 0x01);
  test(0x7fc00000, 0xc0, 0x7f);
  test(0x80000000, 0x80);
}
