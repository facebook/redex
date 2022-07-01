/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodUtil.h"

#include "IRAssembler.h"
#include "RedexTest.h"

class MethodUtilTest : public RedexTest {};

TEST_F(MethodUtilTest, test_count_opcodes) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 6)
    )
  )");

  EXPECT_EQ(6, method::count_opcode_of_types(code.get(), {OPCODE_CONST}));
  code->build_cfg();
  EXPECT_EQ(6, method::count_opcode_of_types(code->cfg(), {OPCODE_CONST}));
  EXPECT_EQ(
      6,
      method::count_opcode_of_types(code->cfg().entry_block(), {OPCODE_CONST}));
  code->clear_cfg();
}
