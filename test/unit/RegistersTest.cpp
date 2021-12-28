/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexInstruction.h"
#include "OpcodeList.h"

static constexpr int kMaxSources = 5;

static void test_opcode(DexOpcode opcode) {
  DexInstruction insn(opcode);
  const std::string text = std::string("for opcode ") + show(opcode);
  const size_t src_count = insn.srcs_size();
  const bool has_dest = (insn.has_dest() > 0);
  const int dest_width =
      has_dest ? dex_opcode::dest_bit_width(insn.opcode()) : 0;
  const bool dest_is_src0 = dex_opcode::dest_is_src(insn.opcode());

  // Populate source test values
  // We want to ensure that setting registers don't stomp each other
  // Create a unique bit pattern for each source based on its idx
  uint16_t dest_value = (1U << dest_width) - 1;
  uint16_t src_values[kMaxSources];
  for (int src_idx = 0; src_idx < src_count; src_idx++) {
    int src_width = dex_opcode::src_bit_width(insn.opcode(), src_idx);
    EXPECT_GE(src_width, 0) << text;
    uint16_t bits = (src_idx + 5);
    bits |= (bits << 4);
    bits |= (bits << 8);
    bits &= ((1U << src_width) - 1);
    src_values[src_idx] = bits;
  }

  // Set test values, and ensure nothing stomps anything else
  if (has_dest) {
    EXPECT_GE(dest_width, 0) << text;
    insn.set_dest(dest_value);
  }
  for (int i = 0; i < src_count; i++) {
    insn.set_src(i, src_values[i]);
  }
  // ensure nothing was stomped, except for what we expect to be stomped
  if (has_dest) {
    EXPECT_EQ(insn.dest(), dest_is_src0 ? src_values[0] : dest_value) << text;
  }
  for (int i = 0; i < src_count; i++) {
    EXPECT_EQ(insn.src(i), src_values[i]) << text;
  }

  // Ensure we can successfully set and then get the min and max register value
  if (has_dest) {
    uint16_t max = (1U << dest_width) - 1;
    insn.set_dest(0);
    EXPECT_EQ(insn.dest(), 0) << text;
    insn.set_dest(max);
    EXPECT_EQ(insn.dest(), max) << text;
  }
  for (int i = 0; i < src_count; i++) {
    uint16_t max = (1U << dex_opcode::src_bit_width(insn.opcode(), i)) - 1;
    insn.set_src(i, 0);
    EXPECT_EQ(insn.src(i), 0) << text;
    insn.set_src(i, max);
    EXPECT_EQ(insn.src(i), max) << text;
  }
}

TEST(Registers, RoundTrip) {
  for (auto op : all_dex_opcodes) {
    test_opcode(op);
  }
}
