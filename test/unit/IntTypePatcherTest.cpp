/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IntTypePatcher.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "ScopeHelper.h"
#include "TypeInference.h"

class IntTypePatcherTest : public RedexTest {};

TEST_F(IntTypePatcherTest, test_int_bool) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()Z"
      (
        (sget "foo;.bar:I;")
        (move-result-pseudo v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:I;")
      (move-result-pseudo v0)
      (if-eqz v0 :b0)

      (const v0 1)
      (return v0)

      (:b0)
      (const v0 0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_int_short) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()S"
      (
        (sget "foo;.bar:I;")
        (move-result-pseudo v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:I;")
      (move-result-pseudo v0)
      (int-to-short v0 v0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_int_char) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()C"
      (
        (sget "foo;.bar:I;")
        (move-result-pseudo v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:I;")
      (move-result-pseudo v0)
      (int-to-char v0 v0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_int_byte) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()B"
      (
        (sget "foo;.bar:I;")
        (move-result-pseudo v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:I;")
      (move-result-pseudo v0)
      (int-to-byte v0 v0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_short_bool) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()Z"
      (
        (sget "foo;.bar:S;")
        (move-result-pseudo v0)
        (int-to-short v0 v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:S;")
      (move-result-pseudo v0)
      (int-to-short v0 v0)
      (if-eqz v0 :b0)

      (const v0 1)
      (return v0)

      (:b0)
      (const v0 0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_char_bool) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()Z"
      (
        (sget "foo;.bar:C;")
        (move-result-pseudo v0)
        (int-to-char v0 v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:C;")
      (move-result-pseudo v0)
      (int-to-char v0 v0)
      (if-eqz v0 :b0)

      (const v0 1)
      (return v0)

      (:b0)
      (const v0 0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_byte_bool) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()Z"
      (
        (sget "foo;.bar:B;")
        (move-result-pseudo v0)
        (int-to-byte v0 v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:B;")
      (move-result-pseudo v0)
      (int-to-byte v0 v0)
      (if-eqz v0 :b0)

      (const v0 1)
      (return v0)

      (:b0)
      (const v0 0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_byte_char) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()C"
      (
        (sget "foo;.bar:B;")
        (move-result-pseudo v0)
        (int-to-byte v0 v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:B;")
      (move-result-pseudo v0)
      (int-to-byte v0 v0)
      (int-to-char v0 v0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_short_char) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()C"
      (
        (sget "foo;.bar:S;")
        (move-result-pseudo v0)
        (int-to-short v0 v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:S;")
      (move-result-pseudo v0)
      (int-to-short v0 v0)
      (int-to-char v0 v0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_char_short) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()S"
      (
        (sget "foo;.bar:C;")
        (move-result-pseudo v0)
        (int-to-char v0 v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:C;")
      (move-result-pseudo v0)
      (int-to-char v0 v0)
      (int-to-short v0 v0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_char_byte) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()B"
      (
        (sget "foo;.bar:C;")
        (move-result-pseudo v0)
        (int-to-char v0 v0)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "foo;.bar:C;")
      (move-result-pseudo v0)
      (int-to-char v0 v0)
      (int-to-byte v0 v0)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_const) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()B"
      (
        (const v0 1)
        (return v0)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (return v0)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_convert_all_blocks) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()Z"
      (
        (const v0 0)
        (if-eqz v0 :b0)

        (sget "foo;.bar:B;")
        (move-result-pseudo v1)
        (int-to-byte v1 v1)
        (return v1)

        (:b0)
        (sget "foo;.bar:B;")
        (move-result-pseudo v1)
        (int-to-byte v1 v1)
        (return v1)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :b0)

      (sget "foo;.bar:B;")
      (move-result-pseudo v1)
      (int-to-byte v1 v1)
      (if-eqz v1 :b1)

      (const v1 1)
      (return v1)

      (:b1)
      (const v1 0)
      (return v1)

      (:b0)
      (sget "foo;.bar:B;")
      (move-result-pseudo v1)
      (int-to-byte v1 v1)
      (if-eqz v1 :b2)

      (const v1 1)
      (return v1)

      (:b2)
      (const v1 0)
      (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(IntTypePatcherTest, test_convert_one_block) {
  auto method = assembler::method_from_string(R"(
    (method (static) "LFoo;.bar:()Z"
      (
        (const v0 0)
        (if-eqz v0 :b0)

        (sget "foo;.bar:B;")
        (move-result-pseudo v1)
        (int-to-byte v1 v1)
        (return v1)

        (:b0)
        (const v1 1)
        (return v1)
      )
    )
  )");
  IRCode* code = method->get_code();
  code->build_cfg();
  IntTypePatcherPass patcher;
  patcher.run(method);
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :b0)

      (sget "foo;.bar:B;")
      (move-result-pseudo v1)
      (int-to-byte v1 v1)
      (if-eqz v1 :b1)

      (const v1 1)
      (return v1)

      (:b1)
      (const v1 0)
      (return v1)

      (:b0)
      (const v1 1)
      (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()));
}
