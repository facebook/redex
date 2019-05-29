/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CommonSubexpressionElimination.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "VirtualScope.h"

class CommonSubexpressionEliminationTest : public RedexTest {
 public:
  CommonSubexpressionEliminationTest() {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    get_vmethods(get_object_type());
  }
};

void test(const Scope& scope,
          const std::string& code_str,
          const std::string& expected_str,
          size_t expected_instructions_eliminated,
          size_t expected_inlined_barriers_into_methods = 0,
          size_t expected_inlined_barriers_iterations = 1,
          size_t max_iterations = 10) {
  auto field_a = static_cast<DexField*>(DexField::make_field("LFoo;.a:I"));
  field_a->make_concrete(ACC_PUBLIC);

  auto field_b = static_cast<DexField*>(DexField::make_field("LFoo;.b:I"));
  field_b->make_concrete(ACC_PUBLIC);

  auto field_s = static_cast<DexField*>(DexField::make_field("LFoo;.s:I"));
  field_s->make_concrete(ACC_PUBLIC | ACC_STATIC);

  auto field_t = static_cast<DexField*>(DexField::make_field("LFoo;.t:I"));
  field_t->make_concrete(ACC_PUBLIC | ACC_STATIC);

  auto field_v = static_cast<DexField*>(DexField::make_field("LFoo;.v:I"));
  field_v->make_concrete(ACC_PUBLIC | ACC_VOLATILE);

  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  code.get()->build_cfg(/* editable */ true);
  CommonSubexpressionElimination::SharedState shared_state;
  auto method_barriers_stats =
      shared_state.init_method_barriers(scope, max_iterations);
  CommonSubexpressionElimination cse(&shared_state, code.get()->cfg());
  bool is_static = true;
  DexType* declaring_type = nullptr;
  DexTypeList* args = DexTypeList::make_type_list({});
  cse.patch(is_static, declaring_type, args);
  code.get()->clear_cfg();
  auto stats = cse.get_stats();

  EXPECT_EQ(expected_instructions_eliminated, stats.instructions_eliminated)
      << assembler::to_string(code.get()).c_str();

  EXPECT_EQ(expected_inlined_barriers_into_methods,
            method_barriers_stats.inlined_barriers_into_methods)
      << assembler::to_string(code.get()).c_str();

  EXPECT_EQ(expected_inlined_barriers_iterations,
            method_barriers_stats.inlined_barriers_iterations)
      << assembler::to_string(code.get()).c_str();

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()))
      << assembler::to_string(code.get()).c_str();
};

TEST_F(CommonSubexpressionEliminationTest, simple) {
  auto code_str = R"(
    (
      (const v0 0)
      (add-int v1 v0 v0)
      (add-int v2 v0 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (add-int v1 v0 v0)
      (move v3 v1)
      (add-int v2 v0 v0)
      (move v2 v3)
    )
  )";

  always_assert(get_object_type());
  always_assert(type_class(get_object_type()));
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, pre_values) {
  // By not initializing v0, it will start out as 'top', and a pre-value will
  // be used internally to recover from that situation and still unify the
  // add-int instructions.
  auto code_str = R"(
    (
      (add-int v1 v0 v0)
      (add-int v2 v0 v0)
    )
  )";
  auto expected_str = R"(
    (
      (add-int v1 v0 v0)
      (move v3 v1)
      (add-int v2 v0 v0)
      (move v2 v3)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, many) {
  auto code_str = R"(
    (
      (const v0 0)
      (add-int v1 v0 v0)
      (add-int v2 v0 v0)
      (add-int v3 v0 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (add-int v1 v0 v0)
      (move v4 v1)
      (add-int v2 v0 v0)
      (move v2 v4)
      (add-int v3 v0 v0)
      (move v3 v4)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 2);
}

TEST_F(CommonSubexpressionEliminationTest, registers_dont_matter) {
  auto code_str = R"(
    (
      (const v0 0)
      (const v1 0)
      (add-int v2 v0 v1)
      (add-int v3 v1 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (const v1 0)
      (add-int v2 v0 v1)
      (move v4 v2)
      (add-int v3 v1 v0)
      (move v3 v4)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, commutative) {
  auto code_str = R"(
    (
      (const v0 0)
      (const v1 1)
      (add-int v2 v0 v1)
      (add-int v3 v1 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (const v1 1)
      (add-int v2 v0 v1)
      (move v4 v2)
      (add-int v3 v1 v0)
      (move v3 v4)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, wide) {
  auto code_str = R"(
    (
      (const-wide v0 0)
      (add-long v2 v0 v0)
      (add-long v4 v0 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const-wide v0 0)
      (add-long v2 v0 v0)
      (move-wide v6 v2)
      (add-long v4 v0 v0)
      (move-wide v4 v6)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, object) {
  auto code_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (const-string "hello")
      (move-result-pseudo-object v1)
    )
  )";
  auto expected_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (move-object v2 v0)
      (const-string "hello")
      (move-result-pseudo-object v1)
      (move-object v1 v2)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, iget) {
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, iget_volatile) {
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.v:I")
      (move-result-pseudo v1)
      (iget v0 "LFoo;.v:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = code_str;
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 0);
}

TEST_F(CommonSubexpressionEliminationTest, affected_by_barrier) {
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LWhat;.ever:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = code_str;
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 0);
}

TEST_F(CommonSubexpressionEliminationTest, safe_methods_are_not_barriers) {
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static (v1) "Ljava/lang/Math;.abs:(I)I")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (invoke-static (v1) "Ljava/lang/Math;.abs:(I)I")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, recovery_after_barrier) {
  // at a barrier, the mappings have been reset, but afterwards cse kicks in as
  // expected
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LWhat;.ever:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v3)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LWhat;.ever:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v4 v2)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v3)
      (move v3 v4)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, unaffected_by_barrier) {
  auto code_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (invoke-static () "LWhat;.ever:()V")
      (const-string "hello")
      (move-result-pseudo-object v1)
    )
  )";
  auto expected_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (move-object v2 v0)
      (invoke-static () "LWhat;.ever:()V")
      (const-string "hello")
      (move-result-pseudo-object v1)
      (move-object v1 v2)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, top_move_tracking) {
  auto code_str = R"(
    (
      (move-object v1 v0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (iget v1 "LFoo;.a:I")
      (move-result-pseudo v3)
    )
  )";
  auto expected_str = R"(
    (
      (move-object v1 v0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v4 v2)
      (iget v1 "LFoo;.a:I")
      (move-result-pseudo v3)
      (move v3 v4)
    )
  )";
  test(Scope{type_class(get_object_type())}, code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest,
       empty_non_true_virtual_methods_are_not_barriers) {
  ClassCreator creator(DexType::make_type("LTest0;"));
  creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTest0;.test0:()V"));
  method->make_concrete(ACC_PUBLIC, true /* is_virtual */);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-virtual (v0) "LTest0;.test0:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (invoke-virtual (v0) "LTest0;.test0:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";

  test(Scope{type_class(get_object_type()), creator.create()},
       code_str,
       expected_str,
       1);
}

TEST_F(CommonSubexpressionEliminationTest,
       empty_true_virtual_methods_are_not_barriers) {

  // define base type

  ClassCreator base_creator(DexType::make_type("LTestBase;"));
  base_creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTestBase;.m:()V"));
  method->make_concrete(ACC_PUBLIC, true /* is_virtual */);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  base_creator.add_method(method);
  DexClass* base_class = base_creator.create();

  // define derived type

  ClassCreator derived_creator(DexType::make_type("LTestDerived;"));
  derived_creator.set_super(base_class->get_type());

  method =
      static_cast<DexMethod*>(DexMethod::make_method("LTestDerived;.m:()V"));
  method->make_concrete(ACC_PUBLIC, true /* is_virtual */);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  derived_creator.add_method(method);
  DexClass* derived_class = derived_creator.create();

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-virtual (v0) "LTestBase;.m:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (invoke-virtual (v0) "LTestBase;.m:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";

  test(Scope{type_class(get_object_type()), base_class, derived_class},
       code_str,
       expected_str,
       1);
}

TEST_F(CommonSubexpressionEliminationTest,
       non_empty_overriding_virtual_methods_are_barriers) {

  // define base type

  ClassCreator base_creator(DexType::make_type("LTestBase;"));
  base_creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTestBase;.m:()V"));
  method->make_concrete(ACC_PUBLIC, true /* is_virtual */);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  base_creator.add_method(method);
  DexClass* base_class = base_creator.create();

  // define derived type

  ClassCreator derived_creator(DexType::make_type("LTestDerived;"));
  derived_creator.set_super(base_class->get_type());

  method =
      static_cast<DexMethod*>(DexMethod::make_method("LTestDerived;.m:()V"));
  method->make_concrete(ACC_PUBLIC, true /* is_virtual */);
  method->set_code(assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (iput v0 v1 "LFoo;.a:I")
      (return-void)
    )
  )"));
  derived_creator.add_method(method);
  DexClass* derived_class = derived_creator.create();

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-virtual (v0) "LTestBase;.m:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = code_str;

  test(Scope{type_class(get_object_type()), base_class, derived_class},
       code_str,
       expected_str,
       0);
}

TEST_F(CommonSubexpressionEliminationTest,
       empty_static_methods_are_not_barriers) {
  ClassCreator creator(DexType::make_type("LTest1;"));
  creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTest1;.test1:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LTest1;.test1:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (invoke-static () "LTest1;.test1:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";

  test(Scope{type_class(get_object_type()), creator.create()},
       code_str,
       expected_str,
       1);
}

TEST_F(CommonSubexpressionEliminationTest, benign_after_inlining_once) {
  ClassCreator a_creator(DexType::make_type("LA;"));
  a_creator.set_super(get_object_type());

  auto method = static_cast<DexMethod*>(DexMethod::make_method("LA;.m:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
     (
       (const v0 0)
       (iget v0 "LFoo;.a:I")
       (move-result-pseudo v1)
       (invoke-static () "LB;.m:()V")
       (iget v0 "LFoo;.a:I")
       (move-result-pseudo v2)
     )
   )"));
  a_creator.add_method(method);

  ClassCreator b_creator(DexType::make_type("LB;"));
  b_creator.set_super(get_object_type());

  method = static_cast<DexMethod*>(DexMethod::make_method("LB;.m:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  b_creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LA;.m:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (invoke-static () "LA;.m:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";

  test(Scope{type_class(get_object_type()), a_creator.create(),
             b_creator.create()},
       code_str, expected_str, 1, 1, 2);
}

TEST_F(CommonSubexpressionEliminationTest, benign_after_inlining_twice) {
  ClassCreator a_creator(DexType::make_type("LA;"));
  a_creator.set_super(get_object_type());

  auto method = static_cast<DexMethod*>(DexMethod::make_method("LA;.m:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
     (
       (const v0 0)
       (iget v0 "LFoo;.a:I")
       (move-result-pseudo v1)
       (invoke-static () "LB;.m:()V")
       (iget v0 "LFoo;.a:I")
       (move-result-pseudo v2)
     )
   )"));
  a_creator.add_method(method);

  ClassCreator b_creator(DexType::make_type("LB;"));
  b_creator.set_super(get_object_type());

  method = static_cast<DexMethod*>(DexMethod::make_method("LB;.m:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
     (
       (const v0 0)
       (iget v0 "LFoo;.a:I")
       (move-result-pseudo v1)
       (invoke-static () "LC;.m:()V")
       (iget v0 "LFoo;.a:I")
       (move-result-pseudo v2)
     )
   )"));
  b_creator.add_method(method);

  ClassCreator c_creator(DexType::make_type("LC;"));
  c_creator.set_super(get_object_type());

  method = static_cast<DexMethod*>(DexMethod::make_method("LC;.m:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string("((return-void))"));
  c_creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LA;.m:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (invoke-static () "LA;.m:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";

  test(Scope{type_class(get_object_type()), a_creator.create(),
             b_creator.create(), c_creator.create()},
       code_str, expected_str, 1, 2, 3);
}

TEST_F(CommonSubexpressionEliminationTest, not_benign_after_inlining_once) {
  ClassCreator a_creator(DexType::make_type("LA;"));
  a_creator.set_super(get_object_type());

  auto method = static_cast<DexMethod*>(DexMethod::make_method("LA;.m:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
     (
       (const v0 0)
       (iget v0 "LFoo;.a:I")
       (move-result-pseudo v1)
       (invoke-static () "LB;.m:()V")
       (iget v0 "LFoo;.a:I")
       (move-result-pseudo v2)
     )
   )"));
  a_creator.add_method(method);

  ClassCreator b_creator(DexType::make_type("LB;"));
  b_creator.set_super(get_object_type());

  method = static_cast<DexMethod*>(DexMethod::make_method("LB;.m:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (iput v0 v1 "LFoo;.a:I")
      (return-void)
    )
  )"));
  b_creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LA;.m:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = code_str;

  test(Scope{type_class(get_object_type()), a_creator.create(),
             b_creator.create()},
       code_str, expected_str, 0, 1, 2);
}

TEST_F(CommonSubexpressionEliminationTest,
       invoked_static_method_with_relevant_i_barrier) {
  ClassCreator creator(DexType::make_type("LTest2;"));
  creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTest2;.test2:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (iput v0 v1 "LFoo;.a:I")
      (return-void)
    )
  )"));
  creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LTest2;.test2:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = code_str;

  test(Scope{type_class(get_object_type()), creator.create()},
       code_str,
       expected_str,
       0);
}

TEST_F(CommonSubexpressionEliminationTest,
       invoked_static_method_with_relevant_s_barrier) {
  ClassCreator creator(DexType::make_type("LTest3;"));
  creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTest3;.test3:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (sput v0 "LFoo;.s:I")
      (return-void)
    )
  )"));
  creator.add_method(method);

  auto code_str = R"(
    (
      (sget "LFoo;.s:I")
      (move-result-pseudo v0)
      (invoke-static () "LTest3;.test3:()V")
      (sget "LFoo;.s:I")
      (move-result-pseudo v1)
    )
  )";
  auto expected_str = code_str;

  test(Scope{type_class(get_object_type()), creator.create()},
       code_str,
       expected_str,
       0);
}

TEST_F(CommonSubexpressionEliminationTest,
       invoked_static_method_with_relevant_a_barrier) {
  ClassCreator creator(DexType::make_type("LTest4;"));
  creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTest4;.test4:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (const v2 0)
      (aput v0 v1 v2)
      (return-void)
    )
  )"));
  creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (const v1 0)
      (aget v0 v1)
      (move-result-pseudo v2)
      (invoke-static () "LTest4;.test4:()V")
      (aget v0 v1)
      (move-result-pseudo v3)
    )
  )";
  auto expected_str = code_str;

  test(Scope{type_class(get_object_type()), creator.create()},
       code_str,
       expected_str,
       0);
}

TEST_F(CommonSubexpressionEliminationTest,
       invoked_static_method_with_irrelevant_i_barrier) {
  ClassCreator creator(DexType::make_type("LTest2;"));
  creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTest2;.test2:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (iput v0 v1 "LFoo;.b:I")
      (return-void)
    )
  )"));
  creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LTest2;.test2:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (invoke-static () "LTest2;.test2:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";

  test(Scope{type_class(get_object_type()), creator.create()},
       code_str,
       expected_str,
       1);
}

TEST_F(CommonSubexpressionEliminationTest,
       invoked_static_method_with_irrelevant_s_barrier) {
  ClassCreator creator(DexType::make_type("LTest5;"));
  creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTest5;.test5:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (sput v0 "LFoo;.s:I")
      (return-void)
    )
  )"));
  creator.add_method(method);

  auto code_str = R"(
    (
      (sget "LFoo;.t:I")
      (move-result-pseudo v0)
      (invoke-static () "LTest5;.test5:()V")
      (sget "LFoo;.t:I")
      (move-result-pseudo v1)
    )
  )";
  auto expected_str = R"(
    (
      (sget "LFoo;.t:I")
      (move-result-pseudo v0)
      (move v2 v0)
      (invoke-static () "LTest5;.test5:()V")
      (sget "LFoo;.t:I")
      (move-result-pseudo v1)
      (move v1 v2)
    )
  )";

  test(Scope{type_class(get_object_type()), creator.create()},
       code_str,
       expected_str,
       1);
}

TEST_F(CommonSubexpressionEliminationTest,
       invoked_static_method_with_irrelevant_a_barrier) {
  ClassCreator creator(DexType::make_type("LTest6;"));
  creator.set_super(get_object_type());

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LTest6;.test6:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  method->set_code(assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (const v2 0)
      (aput-object v0 v1 v2)
      (return-void)
    )
  )"));
  creator.add_method(method);

  auto code_str = R"(
    (
      (const v0 0)
      (const v1 0)
      (aget v0 v1)
      (move-result-pseudo v2)
      (invoke-static () "LTest6;.test6:()V")
      (aget v0 v1)
      (move-result-pseudo v3)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (const v1 0)
      (aget v0 v1)
      (move-result-pseudo v2)
      (move v4 v2)
      (invoke-static () "LTest6;.test6:()V")
      (aget v0 v1)
      (move-result-pseudo v3)
      (move v3 v4)
    )
  )";

  test(Scope{type_class(get_object_type()), creator.create()},
       code_str,
       expected_str,
       1);
}
