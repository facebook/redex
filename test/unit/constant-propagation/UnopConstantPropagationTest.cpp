/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

TEST_F(ConstantPropagationTest, UnopNegIntFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (neg-int v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (const v1 -1)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopNegLongFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 9223372036854775807)
      (neg-long v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 9223372036854775807)
      (const-wide v1 -9223372036854775807)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToLongFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -2147483647)
      (int-to-long v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -2147483647)
      (const-wide v1 -2147483647)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToByteFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (int-to-byte v1 v0)

      (const v2 128)
      (int-to-byte v3 v2)

      (const v4 -129)
      (int-to-byte v5 v4)

      (const v6 -2147483648)
      (int-to-byte v7 v6)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (const v1 -1)

      (const v2 128)
      (const v3 -128)

      (const v4 -129)
      (const v5 127)

      (const v6 -2147483648)
      (const v7 0)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToCharFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -65535)
      (int-to-char v1 v0)

      (const v2 2147483647)
      (int-to-char v3 v2)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -65535)
      (const v1 1)

      (const v2 2147483647)
      (const v3 65535)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopIntToShortFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (int-to-short v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 2147483647)
      (const v1 -1)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantPropagationTest, UnopLongToIntFolding) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 -9223372036854775807)
      (long-to-int v1 v0)

      (return v1)
    )
   )");
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer(),
                cp::Transform::Config());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 -9223372036854775807)
      (const v1 1)

      (return v1)
    )
    )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
