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

TEST(IRAssembler, disassembleCode) {
  g_redex = new RedexContext();

  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     :foo-label
     (if-eqz v0 :foo-label)
     (invoke-virtual (v0 v1) "LFoo;.bar:(II)V")
     (sget-object "LFoo;.qux:LBar;")
     (move-result-pseudo-object v0)
     (return-void)
    )
)");
  EXPECT_EQ(code->get_registers_size(), 2);

  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
            "((const v0 0) "
            ":L0 "
            "(if-eqz v0 :L0) "
            "(invoke-virtual (v0 v1) \"LFoo;.bar:(II)V\") "
            "(sget-object \"LFoo;.qux:LBar;\") "
            "(move-result-pseudo-object v0) "
            "(return-void))");
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));

  delete g_redex;
}

TEST(IRAssembler, empty) {
  auto code = assembler::ircode_from_string(R"((
    (return-void)
  ))");
  EXPECT_EQ(code->get_registers_size(), 0);
}

TEST(IRAssembler, assembleMethod) {
  g_redex = new RedexContext();

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

  delete g_redex;
}
