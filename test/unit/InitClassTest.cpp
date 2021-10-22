/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CFGInliner.h"
#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "Show.h"

void test_init_class(const std::string& original_str,
                     const std::string& expected_str) {
  auto original_code = assembler::ircode_from_string(original_str);
  original_code->build_cfg(true);
  auto& original = original_code->cfg();

  auto expected_code = assembler::ircode_from_string(expected_str);

  const std::string& final_cfg = show(original);
  original_code->clear_cfg();
  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(original_code.get()))
      << final_cfg;
}

class InitClassTest : public RedexTest {};

TEST_F(InitClassTest, simple) {
  const auto original_str = R"(
    (
      (init-class "LCls;")
      (return-void)
    )
  )";
  const auto expected_str = R"(
    (
      (init-class "LCls;")
      (return-void)
    )
  )";
  test_init_class(original_str, expected_str);
}
