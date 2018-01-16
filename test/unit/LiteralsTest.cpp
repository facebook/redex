/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexInstruction.h"

// Tests getting and setting literal value of a single opcode
// width is the number of value bits of the stored literal,
// while lshift_amt is how many bits that value is expected to be shifted left
// lshift_amt is only nonzero for high opcodes
static void test_1_opcode(const char* name,
                          DexOpcode opcode,
                          int width,
                          int lshift) {
  DexInstruction insn(opcode);
  const int src_count = insn.srcs_size();
  int64_t min = -(1 << (width - 1));
  int64_t max = -min - 1;
  auto perform_1_test = [&](int64_t value) {
    int64_t ext_value = value << lshift;
    insn.set_literal(ext_value);
    // set the src and dst registers to verify they don't stomp the literal
    for (int i = 0; i < src_count; i++) {
      insn.set_src(i, i);
    }
    insn.set_dest(0);
    EXPECT_EQ(insn.get_literal(), ext_value)
        << "for opcode " << name << " value " << value;
  };
  // perform no more than 256 tests
  // note: naive 1LL<<width may overflow
  int64_t stride = width <= 8 ? 1LL : (1LL << (width - 8));
  for (int64_t value = min; value < max; value += stride) {
    perform_1_test(value);
  }
  // Always check the min, max, and 0 values
  perform_1_test(min);
  perform_1_test(0);
  perform_1_test(max);
}

#define TEST_1_OPCODE(code, width) \
  test_1_opcode(#code, DOPCODE_##code, width, 0)
#define TEST_1_HI_OPCODE(code, width, lshift) \
  test_1_opcode(#code, DOPCODE_##code, width, lshift)

TEST(LiteralRoundTrip, empty) {
  TEST_1_OPCODE(CONST_4, 4);
  TEST_1_OPCODE(CONST_16, 16);
  TEST_1_OPCODE(CONST, 32);
  TEST_1_OPCODE(ADD_INT_LIT16, 16);
  TEST_1_OPCODE(RSUB_INT, 16);
  TEST_1_OPCODE(MUL_INT_LIT16, 16);
  TEST_1_OPCODE(DIV_INT_LIT16, 16);
  TEST_1_OPCODE(REM_INT_LIT16, 16);
  TEST_1_OPCODE(AND_INT_LIT16, 16);
  TEST_1_OPCODE(OR_INT_LIT16, 16);
  TEST_1_OPCODE(XOR_INT_LIT16, 16);
  TEST_1_OPCODE(ADD_INT_LIT8, 8);
  TEST_1_OPCODE(RSUB_INT_LIT8, 8);
  TEST_1_OPCODE(MUL_INT_LIT8, 8);
  TEST_1_OPCODE(DIV_INT_LIT8, 8);
  TEST_1_OPCODE(REM_INT_LIT8, 8);
  TEST_1_OPCODE(AND_INT_LIT8, 8);
  TEST_1_OPCODE(OR_INT_LIT8, 8);
  TEST_1_OPCODE(XOR_INT_LIT8, 8);
  TEST_1_OPCODE(SHL_INT_LIT8, 8);
  TEST_1_OPCODE(SHR_INT_LIT8, 8);
  TEST_1_OPCODE(USHR_INT_LIT8, 8);

  TEST_1_OPCODE(CONST_WIDE_16, 16);
  TEST_1_OPCODE(CONST_WIDE_32, 32);
  TEST_1_OPCODE(CONST_WIDE, 64);

  TEST_1_HI_OPCODE(CONST_HIGH16, 16, 16);
  TEST_1_HI_OPCODE(CONST_WIDE_HIGH16, 16, 48);
}
