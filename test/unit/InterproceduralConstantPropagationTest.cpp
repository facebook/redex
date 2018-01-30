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
#include "Walkers.h"

bool operator==(const ConstantEnvironment& a, const ConstantEnvironment& b) {
  return a.equals(b);
}

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
     (const v1 0)
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
     (const v0 0)
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
     (const v0 0)
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
     (const v1 0)
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
     (const v1 1)
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
     (const v0 0)
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
     (const v1 1)
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
     (const v1 2)
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
     (const v0 0)
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
     (const v0 0)
     :label
     (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(m3->get_code()),
            assembler::to_s_expr(expected_code3.get()));
}

// We had a bug where an invoke instruction inside an unreachable block of code
// would cause the whole IPC domain to be set to bottom. This test checks that
// we handle it correctly.
TEST(InterproceduralConstantPropagation, unreachableInvoke) {
  using namespace interprocedural_constant_propagation_impl;

  g_redex = new RedexContext();

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(get_object_type());

  auto m1 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  auto code1 = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (goto :skip)
     (invoke-static (v0) "LFoo;.qux:(I)V") ; this is unreachable
     :skip
     (invoke-static (v0) "LFoo;.baz:(I)V") ; this is reachable
     (return-void)
    )
  )");
  code1->set_registers_size(1);
  m1->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::move(code1), /* is_virtual */ false);
  m1->rstate.set_keep();
  creator.add_method(m1);

  auto m2 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:(I)V"));
  auto code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (return-void)
    )
  )");
  code2->set_registers_size(2);
  m2->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::move(code2), /* is_virtual */ false);
  creator.add_method(m2);

  auto m3 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.qux:(I)V"));
  auto code3 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (return-void)
    )
  )");
  code3->set_registers_size(2);
  m3->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::move(code3), /* is_virtual */ false);
  creator.add_method(m3);

  auto cls = creator.create();
  scope.push_back(cls);

  call_graph::Graph cg(scope, /* include_virtuals */ false);
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg();
  });
  ConstPropConfig config;
  FixpointIterator fp_iter(cg, config);
  fp_iter.run({{INPUT_ARGS, ArgumentDomain()}});

  // Check m2 is reachable, despite m3 being unreachable
  EXPECT_EQ(fp_iter.get_entry_state_at(m2).get(INPUT_ARGS),
            ArgumentDomain({{0, SignedConstantDomain(0)}}));
  EXPECT_TRUE(fp_iter.get_entry_state_at(m3).is_bottom());

  delete g_redex;
}

struct RuntimeInputCheckTest : testing::Test {
  DexMethodRef* m_fail_handler;

  RuntimeInputCheckTest() {
    g_redex = new RedexContext();
    m_fail_handler = DexMethod::make_method(
        "Lcom/facebook/redex/ConstantPropagationAssertHandler;.fail:(I)V");
  }

  ~RuntimeInputCheckTest() {
    delete g_redex;
  }
};

TEST_F(RuntimeInputCheckTest, RuntimeInputEqualityCheck) {
  using interprocedural_constant_propagation_impl::insert_runtime_input_checks;

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (return-void)
    )
  )");
  code->set_registers_size(1);
  DexMethod* method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:(I)V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, std::move(code), false);

  ConstantEnvironment env{{0, SignedConstantDomain(5)}};
  insert_runtime_input_checks(env, m_fail_handler, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (const v1 5)
      (if-eq v0 v1 :assertion-true)
      (const v2 0)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.fail:(I)V")
      :assertion-true
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(method->get_code()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RuntimeInputCheckTest, RuntimeInputSignCheck) {
  using interprocedural_constant_propagation_impl::insert_runtime_input_checks;
  using sign_domain::Interval;

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (return-void)
    )
  )");
  code->set_registers_size(2);
  DexMethod* method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:(II)V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, std::move(code), false);

  ConstantEnvironment env{{0, SignedConstantDomain(Interval::GEZ)},
                          {1, SignedConstantDomain(Interval::LTZ)}};
  insert_runtime_input_checks(env, m_fail_handler, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (if-gez v0 :assertion-true-1)
      (const v2 0)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.fail:(I)V")
      :assertion-true-1
      (if-ltz v1 :assertion-true-2)
      (const v3 1)
      (invoke-static (v3) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.fail:(I)V")
      :assertion-true-2
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(method->get_code()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RuntimeInputCheckTest, RuntimeInputCheckIntOnly) {
  using interprocedural_constant_propagation_impl::insert_runtime_input_checks;
  using sign_domain::Interval;

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0) ; long -- we don't handle this yet
      (load-param v1) ; int
      (return-void)
    )
  )");
  code->set_registers_size(2);
  DexMethod* method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:(JI)V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, std::move(code), false);

  ConstantEnvironment env{{0, SignedConstantDomain(Interval::GEZ)},
                          {1, SignedConstantDomain(Interval::LTZ)}};
  insert_runtime_input_checks(env, m_fail_handler, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (if-ltz v1 :assertion-true-1)
      (const v2 1)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.fail:(I)V")
      :assertion-true-1
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(method->get_code()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RuntimeInputCheckTest, RuntimeInputCheckVirtualMethod) {
  using interprocedural_constant_propagation_impl::insert_runtime_input_checks;
  using sign_domain::Interval;

  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0) ; `this` argument
      (load-param v1)
      (return-void)
    )
  )");
  code->set_registers_size(2);
  DexMethod* method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:(I)V"));
  method->make_concrete(ACC_PUBLIC, std::move(code), true);

  ConstantEnvironment env{{1, SignedConstantDomain(Interval::LTZ)}};
  insert_runtime_input_checks(env, m_fail_handler, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0) ; `this` argument
      (load-param v1)
      (if-ltz v1 :assertion-true-1)
      (const v2 0)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.fail:(I)V")
      :assertion-true-1
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(method->get_code()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(InterproceduralConstantPropagation, nonConstantValueField) {
  g_redex = new RedexContext();

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(get_object_type());

  auto field = static_cast<DexField*>(DexField::make_field("LFoo;.qux:I"));
  field->make_concrete(ACC_PUBLIC | ACC_STATIC,
                       new DexEncodedValueBit(DEVT_INT, 1));
  creator.add_field(field);

  auto m1 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  auto code1 = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (sput v0 "LFoo;.qux:I")
     (return-void)
    )
  )");
  code1->set_registers_size(1);
  m1->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::move(code1), /* is_virtual */ false);
  m1->rstate.set_keep(); // Make this an entry point
  creator.add_method(m1);

  auto m2 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:()V"));
  auto code2 = assembler::ircode_from_string(R"(
    (
     (sget "LFoo;.qux:I")
     (move-result-pseudo v0)
     (if-nez v0 :label)
     (const v0 0)
     :label
     (return-void)
    )
  )");
  code2->set_registers_size(1);
  m2->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::move(code2), /* is_virtual */ false);
  m2->rstate.set_keep(); // Make this an entry point
  creator.add_method(m2);

  auto cls = creator.create();
  scope.push_back(cls);
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg();
  });

  ConstPropConfig config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(scope);

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (goto :label)
     (const v0 0)
     :label
     (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(m2->get_code()),
            assembler::to_s_expr(expected_code2.get()));

  delete g_redex;
}

TEST(InterproceduralConstantPropagation, constantValueField) {
  g_redex = new RedexContext();

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(get_object_type());

  auto field = static_cast<DexField*>(DexField::make_field("LFoo;.qux:I"));
  field->make_concrete(ACC_PUBLIC | ACC_STATIC,
                       new DexEncodedValueBit(DEVT_INT, 1));
  creator.add_field(field);

  auto m1 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  auto code1 = assembler::ircode_from_string(R"(
    (
     (const v0 0) ; this differs from the original encoded value of Foo.qux
     (sput v0 "LFoo;.qux:I")
     (return-void)
    )
  )");
  code1->set_registers_size(1);
  m1->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::move(code1), /* is_virtual */ false);
  m1->rstate.set_keep(); // Make this an entry point
  creator.add_method(m1);

  auto m2 = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:()V"));
  auto code2 = assembler::ircode_from_string(R"(
    (
     (sget "LFoo;.qux:I")
     (move-result-pseudo v0)
     (if-nez v0 :label)
     (const v0 0)
     :label
     (return-void)
    )
  )");
  code2->set_registers_size(1);
  m2->make_concrete(
      ACC_PUBLIC | ACC_STATIC, std::move(code2), /* is_virtual */ false);
  m2->rstate.set_keep(); // Make this an entry point
  creator.add_method(m2);

  auto cls = creator.create();
  scope.push_back(cls);
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg();
  });

  ConstPropConfig config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(scope);

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (sget "LFoo;.qux:I")
     (move-result-pseudo v0)
     (if-nez v0 :label)
     (const v0 0)
     :label
     (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(m2->get_code()),
            assembler::to_s_expr(expected_code2.get()));

  delete g_redex;
}
