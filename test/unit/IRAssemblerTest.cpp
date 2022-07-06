/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRAssembler.h"

#include <gtest/gtest.h>

#include "DexPosition.h"
#include "RedexTest.h"
#include "Show.h"

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

TEST_F(IRAssemblerTest, assembleClassWithMethod) {
  auto method = assembler::class_with_method("LFoo;",
                                             R"(
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
}

TEST_F(IRAssemblerTest, assembleClassWithMethods) {
  const std::vector<DexMethod*>& methods = {
      assembler::method_from_string(R"(
        (method (private) "LFoo;.bar0:(I)V"
          (
            (return-void)
          )
        )
      )"),
      assembler::method_from_string(R"(
        (method (public) "LFoo;.bar1:(V)V"
          (
            (return-void)
          )
        )
      )"),
  };

  auto clazz = assembler::class_with_methods("LFoo;", methods);

  DexMethod* method0 = clazz->get_dmethods().at(0);
  EXPECT_EQ(method0->get_access(), ACC_PRIVATE);
  EXPECT_STREQ(method0->get_name()->c_str(), "bar0");
  EXPECT_STREQ(method0->get_class()->get_name()->c_str(), "LFoo;");
  EXPECT_EQ(assembler::to_string(method0->get_code()), "((return-void))");

  DexMethod* method1 = clazz->get_vmethods().at(0);
  EXPECT_EQ(method1->get_access(), ACC_PUBLIC);
  EXPECT_STREQ(method1->get_name()->c_str(), "bar1");
  EXPECT_STREQ(method1->get_class()->get_name()->c_str(), "LFoo;");
  EXPECT_EQ(assembler::to_string(method1->get_code()), "((return-void))");
}

TEST_F(IRAssemblerTest, use_switch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (switch v0 (:a :b :c))
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
            "((switch v0 (:L0 :L1 :L2)) "
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
      (switch v0 (:a :b :c))
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
            "((switch v0 (:L1 :L2 :L3)) "
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
      (switch v1 (:a :b))
      (switch v0 (:a :b))

      (:a 0)
      (const v0 0)

      (:b 1)
      (const v1 1)
    )
  )");

  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
            "((switch v1 (:L0 :L1)) "
            "(switch v0 (:L0 :L1)) "

            "(:L0 0) "
            "(const v0 0) "

            "(:L1 1) "
            "(const v1 1))");

  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));
}

TEST_F(IRAssemblerTest, diabolical_bad_order_switch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (switch v0 (:b :a))

      (:a 0)
      (const v0 0)

      (:b 1)
      (const v1 1)
    )
  )");

  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
            "((switch v0 (:L0 :L1)) "

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
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

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

TEST_F(IRAssemblerTest, posWithParent_DbgLabel) {
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method2 = DexMethod::make_method("LFoo;.baz:()I")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
     (.pos:dbg_0 "LFoo;.bar:()V" "Foo.java" 420)
     (.pos:dbg_1 "LFoo;.baz:()I" "Foo.java" 440 dbg_0)
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

TEST_F(IRAssemblerTest, posWithParent_UserLabel) {
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method2 = DexMethod::make_method("LFoo;.baz:()I")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
     (.pos:DarthVader "LFoo;.bar:()V" "Foo.java" 420)
     (.pos:LukeSkywalker "LFoo;.baz:()I" "Foo.java" 440 DarthVader)
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

TEST_F(IRAssemblerTest, posWithParent_BadParent) {
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method2 = DexMethod::make_method("LFoo;.baz:()I")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
     (.pos:Bob "LFoo;.bar:()V" "Foo.java" 420)
     (.pos:John "LFoo;.baz:()I" "Foo.java" 440 BadParent)
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
  EXPECT_EQ(pos1->parent, nullptr);
}

TEST_F(IRAssemblerTest, posWithGrandparent) {
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method2 = DexMethod::make_method("LFoo;.baz:()I")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method3 = DexMethod::make_method("LFoo;.baz:()Z")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
     (.pos:dbg_0 "LFoo;.bar:()V" "Foo.java" 420)
     (.pos:dbg_1 "LFoo;.baz:()I" "Foo.java" 440 dbg_0)
     (.pos:dbg_2 "LFoo;.baz:()Z" "Foo.java" 441 dbg_1)
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
  ASSERT_EQ(positions.size(), 3);

  auto pos0 = positions[0];
  EXPECT_EQ(show(pos0->method), std::string("LFoo;.bar:()V"));
  EXPECT_EQ(pos0->file->c_str(), std::string("Foo.java"));
  EXPECT_EQ(pos0->line, 420);
  EXPECT_EQ(pos0->parent, nullptr);

  auto pos2 = positions[2];
  EXPECT_EQ(show(pos2->method), std::string("LFoo;.baz:()Z"));
  EXPECT_EQ(pos2->file->c_str(), std::string("Foo.java"));
  EXPECT_EQ(pos2->line, 441);
  EXPECT_EQ(*pos2->parent->parent, *pos0);
}

TEST_F(IRAssemblerTest, posWithGreatGrandparent) {
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method2 = DexMethod::make_method("LFoo;.baz:()I")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method3 = DexMethod::make_method("LFoo;.baz:()Z")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
     (.pos:dbg_0 "LFoo;.bar:()V" "Foo.java" 420)
     (.pos:dbg_1 "LFoo;.baz:()I" "Foo.java" 440 dbg_0)
     (.pos:dbg_2 "LFoo;.baz:()Z" "Foo.java" 441 dbg_1)
     (.pos:dbg_3 "LFoo;.baz:()Z" "Foo.java" 442 dbg_2)
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
  ASSERT_EQ(positions.size(), 4);

  auto pos0 = positions[0];
  EXPECT_EQ(show(pos0->method), std::string("LFoo;.bar:()V"));
  EXPECT_EQ(pos0->file->c_str(), std::string("Foo.java"));
  EXPECT_EQ(pos0->line, 420);
  EXPECT_EQ(pos0->parent, nullptr);

  auto pos3 = positions[3];
  EXPECT_EQ(show(pos3->method), std::string("LFoo;.baz:()Z"));
  EXPECT_EQ(pos3->file->c_str(), std::string("Foo.java"));
  EXPECT_EQ(pos3->line, 442);
  EXPECT_EQ(*pos3->parent->parent->parent, *pos0);
}

std::vector<DexDebugInstruction*> get_debug_info(
    const std::unique_ptr<IRCode>& code) {
  std::vector<DexDebugInstruction*> debug_info;
  for (const auto& mie : *code) {
    if (mie.type == MFLOW_DEBUG) {
      debug_info.push_back(mie.dbgop.get());
    }
  }
  return debug_info;
}

TEST_F(IRAssemblerTest, dexDebugInstruction) {
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
      (.dbg DBG_SET_FILE "foo.java")
      (.dbg DBG_SET_EPILOGUE_BEGIN)
      (.dbg DBG_SET_PROLOGUE_END)
      (.dbg DBG_RESTART_LOCAL 1)
      (.dbg DBG_END_LOCAL 2)
      (.dbg DBG_START_LOCAL_EXTENDED 3 "name" "Ljava/lang/Objects;" "sig")
      (.dbg DBG_START_LOCAL 4 "name" "Ljava/lang/Objects;")
      (.dbg DBG_ADVANCE_LINE 5)
      (.dbg DBG_ADVANCE_PC 6)
      (.dbg DBG_END_SEQUENCE)
      (.dbg EMIT 10)
      (const v0 42)
      (return v0)
    )
  )");

  // Ensure serialization works as expected
  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));

  // Ensure deserialization works as expected
  EXPECT_EQ(code->count_opcodes(), 2);
  auto debug_info = get_debug_info(code);
  EXPECT_EQ(debug_info.size(), 11);

  auto dbg0 = debug_info[0];
  EXPECT_EQ(dbg0->opcode(), DBG_SET_FILE);
  auto dbg0_ = dynamic_cast<DexDebugOpcodeSetFile*>(dbg0);
  EXPECT_NE(dbg0_, nullptr);
  EXPECT_EQ(dbg0_->file()->str(), "foo.java");

  auto dbg1 = debug_info[1];
  EXPECT_EQ(dbg1->opcode(), DBG_SET_EPILOGUE_BEGIN);
  EXPECT_EQ(dbg1->uvalue(), DEX_NO_INDEX);

  auto dbg2 = debug_info[2];
  EXPECT_EQ(dbg2->opcode(), DBG_SET_PROLOGUE_END);
  EXPECT_EQ(dbg2->uvalue(), DEX_NO_INDEX);

  auto dbg3 = debug_info[3];
  EXPECT_EQ(dbg3->opcode(), DBG_RESTART_LOCAL);
  EXPECT_EQ(dbg3->uvalue(), 1);

  auto dbg4 = debug_info[4];
  EXPECT_EQ(dbg4->opcode(), DBG_END_LOCAL);
  EXPECT_EQ(dbg4->uvalue(), 2);

  auto dbg5 = debug_info[5];
  EXPECT_EQ(dbg5->opcode(), DBG_START_LOCAL_EXTENDED);
  auto dbg5_ = dynamic_cast<DexDebugOpcodeStartLocal*>(dbg5);
  EXPECT_NE(dbg5_, nullptr);
  EXPECT_EQ(dbg5_->name()->str(), "name");
  EXPECT_EQ(dbg5_->type()->str(), "Ljava/lang/Objects;");
  EXPECT_EQ(dbg5_->sig()->str(), "sig");

  auto dbg6 = debug_info[6];
  EXPECT_EQ(dbg6->opcode(), DBG_START_LOCAL);
  auto dbg6_ = dynamic_cast<DexDebugOpcodeStartLocal*>(dbg6);
  EXPECT_NE(dbg6_, nullptr);
  EXPECT_EQ(dbg6_->name()->str(), "name");
  EXPECT_EQ(dbg6_->type()->str(), "Ljava/lang/Objects;");
  EXPECT_EQ(dbg6_->sig(), nullptr);

  auto dbg7 = debug_info[7];
  EXPECT_EQ(dbg7->opcode(), DBG_ADVANCE_LINE);
  EXPECT_EQ(dbg7->value(), 5);

  auto dbg8 = debug_info[8];
  EXPECT_EQ(dbg8->opcode(), DBG_ADVANCE_PC);
  EXPECT_EQ(dbg8->uvalue(), 6);

  auto dbg9 = debug_info[9];
  EXPECT_EQ(dbg9->opcode(), DBG_END_SEQUENCE);
  EXPECT_EQ(dbg9->uvalue(), DEX_NO_INDEX);

  auto dbg10 = debug_info[10];
  EXPECT_EQ(dbg10->opcode(), DBG_FIRST_SPECIAL);
  EXPECT_EQ(dbg10->uvalue(), DEX_NO_INDEX);
}
