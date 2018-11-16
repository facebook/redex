/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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

TEST_F(IRAssemblerTest, try_catch_simplest) {
  auto code = assembler::ircode_from_string(R"(
    (
      (.try_start a)
      (const v0 0)
      (.try_end a)

      (.catch (a))
      (const v2 2)
      (return-void)
    )
  )");
  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}

TEST_F(IRAssemblerTest, try_catch_with_next) {
  auto code = assembler::ircode_from_string(R"(
    (
      (.try_start a)
      (const v0 0)
      (.try_end a)

      (.catch (a b) "LFoo;")
      (const v1 1)
      (return-void)

      (.catch (b) "LBar;")
      (const v2 2)
      (return-void)
    )
  )");
  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}

TEST_F(IRAssemblerTest, try_catch_exception_name) {
  auto code1 = assembler::ircode_from_string(R"(
    (
      (.try_start a)
      (const v0 0)
      (.try_end a)

      (.catch (a) "LFoo;")
      (const v1 1)
      (return-void)
    )
  )");
  auto code2 = assembler::ircode_from_string(R"(
    (
      (.try_start a)
      (const v0 0)
      (.try_end a)

      (.catch (a) "LBar;")
      (const v1 1)
      (return-void)
    )
  )");

  EXPECT_NE(assembler::to_string(code1.get()),
            assembler::to_string(code2.get()));
}

TEST_F(IRAssemblerTest, try_catch_with_two_tries) {
  auto code = assembler::ircode_from_string(R"(
    (
      (.try_start a)
      (const v0 0)
      (.try_end a)

      (.try_start a)
      (const v1 1)
      (.try_end a)

      (.catch (a))
      (const v2 2)
      (return-void)
    )
  )");
  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}

std::vector<DexPosition*> get_positions(const std::unique_ptr<IRCode>& code) {
  std::vector<DexPosition*> positions;
  for (const auto& mie : *code) {
    if (mie.type == MFLOW_POSITION) {
      positions.push_back(mie.pos.get());
    }
  }
  return positions;
}

TEST_F(IRAssemblerTest, pos) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (const v0 420)
    )
  )");

  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));

  EXPECT_EQ(code->count_opcodes(), 1);
  auto positions = get_positions(code);
  ASSERT_EQ(positions.size(), 1);
  auto pos = positions[0];
  EXPECT_EQ(show(pos->method), std::string("LFoo;.bar:()V"));
  EXPECT_EQ(pos->file->c_str(), std::string("Foo.java"));
  EXPECT_EQ(pos->line, 420);
  EXPECT_EQ(pos->parent, nullptr);
}

TEST_F(IRAssemblerTest, posWithParent) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method2 =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:()I"));
  method2->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" 420)
     (.pos "LFoo;.baz:()I" "Foo.java" 440 0)
     (const v0 420)
     (return v0)
    )
  )");

  // Ensure serialize + deserialize works as expected
  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));

  // Ensure deserialize actually works
  EXPECT_EQ(code->count_opcodes(), 2);
  auto positions = get_positions(code);
  ASSERT_EQ(positions.size(), 2);

  auto pos0 = positions[0];
  EXPECT_EQ(show(pos0->method), std::string("LFoo;.bar:()V"));
  EXPECT_EQ(pos0->file->c_str(), std::string("Foo.java"));
  EXPECT_EQ(pos0->line, 420);
  EXPECT_EQ(pos0->parent, nullptr);

  auto pos1 = positions[1];
  EXPECT_EQ(show(pos1->method), std::string("LFoo;.baz:()I"));
  EXPECT_EQ(pos1->file->c_str(), std::string("Foo.java"));
  EXPECT_EQ(pos1->line, 440);
  EXPECT_EQ(*pos1->parent, *pos0);
}
