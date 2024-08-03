/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Nopper.h"
#include "RedexTest.h"

class NopperTest : public RedexTest {
 public:
  NopperTest() {}
};

TEST_F(NopperTest, noppable_blocks_insert_nops) {
  auto code_str = R"(
    (
      (load-param v0)
      (if-eqz v0 :L1)
    (:L0)
      (return-void)

    (:L1)
      (return-void)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);
  code->build_cfg();
  auto noppable_blocks = nopper_impl::get_noppable_blocks(code->cfg());
  EXPECT_EQ(code->cfg().blocks().size(), 3);
  EXPECT_EQ(noppable_blocks.size(), 3);

  std::unordered_set<cfg::Block*> set(noppable_blocks.begin(),
                                      noppable_blocks.end());
  nopper_impl::insert_nops(code->cfg(), set);
  code->clear_cfg();

  auto expected_str = R"(
    (
      (load-param v0)
      (nop)
      (if-eqz v0 :L1)
    (:L0)
      (nop)
      (return-void)

    (:L1)
      (nop)
      (return-void)
    )
  )";
  auto expected = assembler::ircode_from_string(expected_str);

  EXPECT_CODE_EQ(code.get(), expected.get());
}

TEST_F(NopperTest, noppable_blocks_exclusions) {
  auto code_str = R"(
    (
      (load-param v0)
    (:L0)
      (goto :L0)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);
  code->build_cfg();
  auto noppable_blocks = nopper_impl::get_noppable_blocks(code->cfg());
  EXPECT_EQ(code->cfg().blocks().size(), 2);
  EXPECT_EQ(noppable_blocks.size(), 0);
}
