/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "RedexTest.h"

struct IRAssemblerTest : public RedexTest {};

TEST_F(IRAssemblerTest, disassembleCode) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (:foo-label)
     (if-eqz v0 :foo-label)
     (invoke-virtual (v0 v1) "LFoo;.bar:(II)V")
     (sget-object "LFoo;.qux:LBar;")
     (move-result-pseudo-object v0)
     (return-void)
    )
)");
  EXPECT_EQ(code->get_registers_size(), 2);

  std::cout << show(code) << std::endl;
  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
            "((const v0 0) "
            "(:L0) "
            "(if-eqz v0 :L0) "
            "(invoke-virtual (v0 v1) \"LFoo;.bar:(II)V\") "
            "(sget-object \"LFoo;.qux:LBar;\") "
            "(move-result-pseudo-object v0) "
            "(return-void))");
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}

TEST_F(IRAssemblerTest, empty) {
  auto code = assembler::ircode_from_string(R"((
    (return-void)
  ))");
  EXPECT_EQ(code->get_registers_size(), 0);
}

TEST_F(IRAssemblerTest, assembleMethod) {
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.bar:(I)V"
     (
      (return-void)
     )
    )
)");
  EXPECT_EQ(method->get_access(), ACC_PRIVATE);
  EXPECT_STREQ(method->get_name()->c_str(), "bar");
  EXPECT_STREQ(method->get_class()->get_name()->c_str(), "LFoo;");
  EXPECT_EQ(assembler::to_string(method->get_code()), "((return-void))");

  auto static_method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:(I)V"
     (
      (return-void)
     )
    )
)");
  EXPECT_EQ(static_method->get_access(), ACC_PUBLIC | ACC_STATIC);
  EXPECT_STREQ(static_method->get_name()->c_str(), "baz");
  EXPECT_STREQ(static_method->get_class()->get_name()->c_str(), "LFoo;");
}

TEST_F(IRAssemblerTest, use_switch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (sparse-switch v0 (:a :b :c))
      (return-void)

      (:a 0)
      (const v0 0)

      (:b 1)
      (const v1 1)

      (:c 2)
      (const v2 2)
    )
  )");

  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
            "((sparse-switch v0 (:L0 :L1 :L2)) "
            "(return-void) "
            "(:L0 0) "
            "(const v0 0) "
            "(:L1 1) "
            "(const v1 1) "
            "(:L2 2) "
            "(const v2 2))");
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}

TEST_F(IRAssemblerTest, use_switch_and_branch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (sparse-switch v0 (:a :b :c))
      (:default)
      (return-void)

      (:a 0)
      (const v0 0)
      (if-eqz v0 :lbl)
      (goto :default)

      (:b 1)
      (const v1 1)
      (goto :default)

      (:c 2)
      (const v2 2)
      (goto :default)

      (const v3 3)
      (goto :default)

      (:lbl)
      (const v4 4)
    )
  )");

  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
      "((sparse-switch v0 (:L1 :L2 :L3)) "
      "(:L0) "
      "(return-void) "

      "(:L1 0) "
      "(const v0 0) "
      "(if-eqz v0 :L4) "
      "(goto :L0) "

      "(:L2 1) "
      "(const v1 1) "
      "(goto :L0) "

      "(:L3 2) "
      "(const v2 2) "
      "(goto :L0) "

      "(const v3 3) "
      "(goto :L0) "

      "(:L4) "
      "(const v4 4))");
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}

TEST_F(IRAssemblerTest, diabolical_double_switch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (sparse-switch v1 (:a :b))
      (sparse-switch v0 (:a :b))

      (:a 0)
      (const v0 0)

      (:b 1)
      (const v1 1)
    )
  )");

  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
            "((sparse-switch v1 (:L0 :L1)) "
            "(sparse-switch v0 (:L0 :L1)) "

            "(:L0 0) "
            "(const v0 0) "

            "(:L1 1) "
            "(const v1 1))");

  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}

TEST_F(IRAssemblerTest, diabolical_bad_order_switch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (sparse-switch v0 (:b :a))

      (:a 0)
      (const v0 0)

      (:b 1)
      (const v1 1)
    )
  )");

  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
            "((sparse-switch v0 (:L0 :L1)) "

            "(:L0 0) "
            "(const v0 0) "

            "(:L1 1) "
            "(const v1 1))");

  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}
