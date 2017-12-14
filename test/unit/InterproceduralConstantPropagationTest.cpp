/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "InterproceduralConstantPropagation.h"

#include <gtest/gtest.h>

#include "Creators.h"
#include "DexUtil.h"
#include "IRAssembler.h"

TEST(InterproceduralConstantPropagation, constantArgument) {
  g_redex = new RedexContext();

   // Let bar() be the only method calling baz(I)V, passing it a constant
   // argument. baz() should be optimized for that constant argument.

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(get_object_type());

  auto m1 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  auto code1 = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; the `this` argument
     (const/4 v1 0)
     (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
     (return-void)
    )
  )");
  code1->set_registers_size(2);
  m1->make_concrete(ACC_PUBLIC, std::move(code1), /* is_virtual */ false);
  m1->rstate.set_keep();
  creator.add_method(m1);

  auto m2 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:(I)V"));
  auto code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; the `this` argument
     (load-param v1)
     (if-eqz v1 :label)
     (const/4 v0 0)
     :label
     (return-void)
    )
  )");
  code2->set_registers_size(2);
  m2->make_concrete(ACC_PUBLIC, std::move(code2), /* is_virtual */ false);
  creator.add_method(m2);

  auto cls = creator.create();
  scope.push_back(cls);
  InterproceduralConstantPropagationPass().run(scope);

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (goto :label)
     (const/4 v0 0)
     :label
     (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(m2->get_code()),
            assembler::to_s_expr(expected_code2.get()));

  delete g_redex;
}

TEST(InterproceduralConstantPropagation, nonConstantArgument) {
  g_redex = new RedexContext();

   // Let there be two methods calling baz(I)V, passing it different arguments.
   // baz() cannot be optimized for a constant argument here.

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(get_object_type());

  auto m1 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.foo:()V"));
  auto code1 = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; the `this` argument
     (const/4 v1 0)
     (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
     (return-void)
    )
  )");
  code1->set_registers_size(2);
  m1->make_concrete(ACC_PUBLIC, std::move(code1), /* is_virtual */ false);
  m1->rstate.set_keep();
  creator.add_method(m1);

  auto m2 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  auto code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; the `this` argument
     (const/4 v1 1)
     (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
     (return-void)
    )
  )");
  code2->set_registers_size(2);
  m2->make_concrete(ACC_PUBLIC, std::move(code2), /* is_virtual */ false);
  m2->rstate.set_keep();
  creator.add_method(m2);

  auto m3 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:(I)V"));
  auto code3 = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; the `this` argument
     (load-param v1)
     (if-eqz v1 :label)
     (const/4 v0 0)
     :label
     (return-void)
    )
  )");
  code3->set_registers_size(2);
  m3->make_concrete(ACC_PUBLIC, std::move(code3), /* is_virtual */ false);
  creator.add_method(m3);

  auto cls = creator.create();
  scope.push_back(cls);

  // m3's code should be unchanged since it cannot be optimized
  auto expected = assembler::to_s_expr(m3->get_code());
  InterproceduralConstantPropagationPass().run(scope);
  EXPECT_EQ(assembler::to_s_expr(m3->get_code()), expected);

  delete g_redex;
}

TEST(InterproceduralConstantPropagation, argumentsGreaterThanZero) {
  g_redex = new RedexContext();

  // Let baz(I)V always be called with arguments > 0. baz() should be
  // optimized for that scenario.

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(get_object_type());

  auto m1 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  auto code1 = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; the `this` argument
     (const/4 v1 1)
     (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
     (return-void)
    )
  )");
  code1->set_registers_size(2);
  m1->make_concrete(ACC_PUBLIC, std::move(code1), /* is_virtual */ false);
  m1->rstate.set_keep();
  creator.add_method(m1);

  auto m2 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar2:()V"));
  auto code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; the `this` argument
     (const/4 v1 2)
     (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
     (return-void)
    )
  )");
  code2->set_registers_size(2);
  m2->make_concrete(ACC_PUBLIC, std::move(code2), /* is_virtual */ false);
  m2->rstate.set_keep();
  creator.add_method(m2);

  auto m3 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:(I)V"));
  auto code3 = assembler::ircode_from_string(R"(
    (
     (load-param v0) ; the `this` argument
     (load-param v1)
     (if-gtz v1 :label)
     (const/4 v0 0)
     :label
     (return-void)
    )
  )");
  code3->set_registers_size(2);
  m3->make_concrete(ACC_PUBLIC, std::move(code3), /* is_virtual */ false);
  creator.add_method(m3);

  auto cls = creator.create();
  scope.push_back(cls);
  InterproceduralConstantPropagationPass().run(scope);

  auto expected_code3 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (goto :label)
     (const/4 v0 0)
     :label
     (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(m3->get_code()),
            assembler::to_s_expr(expected_code3.get()));

  delete g_redex;
}
