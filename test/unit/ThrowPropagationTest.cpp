/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Purity.h"
#include "RedexTest.h"
#include "ThrowPropagationPass.h"
#include "VirtualScope.h"
#include "Walkers.h"

class ThrowPropagationTest : public RedexTest {
 public:
  ThrowPropagationTest() {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    get_vmethods(type::java_lang_Object());
  }
};

void test(const Scope& scope,
          const std::string& code_str,
          const std::string& expected_str) {
  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  ThrowPropagationPass::Config config;
  auto no_return_methods =
      ThrowPropagationPass::get_no_return_methods(config, scope);
  auto override_graph = method_override_graph::build_graph(scope);
  ThrowPropagationPass::run(
      config, no_return_methods, *override_graph, code.get());

  EXPECT_CODE_EQ(code.get(), expected.get());
};

TEST_F(ThrowPropagationTest, dont_change_unknown) {
  auto code_str = R"(
    (
      (invoke-static () "LWhat;.ever:()V")
      (return-void)
    )
  )";
  test(Scope{type_class(type::java_lang_Object())}, code_str, code_str);
}

TEST_F(ThrowPropagationTest, can_return_simple) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (return-void)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       code_str);
}

TEST_F(ThrowPropagationTest, cannot_return_simple) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (throw v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (return-void)
    )
  )";
  auto expected_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (new-instance "Ljava/lang/RuntimeException;")
      (move-result-pseudo-object v0)
      (const-string "Redex: Unreachable code after no-return invoke")
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1) "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
      (throw v0)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       expected_str);
}

TEST_F(ThrowPropagationTest, cannot_return_remove_move_result) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()I")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (throw v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()I")
      (move-result v1)
      (return-void)
    )
  )";
  auto expected_str = R"(
    (
      (invoke-static () "LFoo;.bar:()I")
      (new-instance "Ljava/lang/RuntimeException;")
      (move-result-pseudo-object v2)
      (const-string "Redex: Unreachable code after no-return invoke")
      (move-result-pseudo-object v3)
      (invoke-direct (v2 v3) "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
      (throw v2)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       expected_str);
}

TEST_F(ThrowPropagationTest, cannot_return_simple_already_throws) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (throw v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (const v0 0)
      (throw v0)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       code_str);
}

TEST_F(ThrowPropagationTest, cannot_return_simple_already_does_not_terminate) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (throw v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()V")
      (:b)
      (nop)
      (goto :b)
    )
  )";
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       code_str);
}

TEST_F(ThrowPropagationTest, dont_change_throw_result) {
  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()Ljava/lang/Exception;")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
        (const v0 0)
        (return-object v0)
      )"));
  foo_creator.add_method(method);

  auto code_str = R"(
    (
      (invoke-static () "LFoo;.bar:()Ljava/lang/Exception;")
      (move-result-object v0)
      (throw v0)
    )
  )";
  auto expected_str = code_str;
  test(Scope{type_class(type::java_lang_Object()), foo_creator.create()},
       code_str,
       expected_str);
}
