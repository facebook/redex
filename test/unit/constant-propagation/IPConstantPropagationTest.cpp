/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IPConstantPropagation.h"

#include <signal.h>

#include <gtest/gtest.h>

#include "ConstantPropagationRuntimeAssert.h"
#include "Creators.h"
#include "Debug.h"
#include "DexUtil.h"
#include "IPConstantPropagationAnalysis.h"
#include "IRAssembler.h"
#include "MethodOverrideGraph.h"
#include "RedexTest.h"
#include "VirtualScope.h"
#include "Walkers.h"

using namespace constant_propagation;
using namespace constant_propagation::interprocedural;

struct InterproceduralConstantPropagationTest : public RedexTest {
 public:
  InterproceduralConstantPropagationTest() {
    // Calling get_vmethods under the hood initializes the object-class, which
    // we need in the tests to create a proper scope
    get_vmethods(type::java_lang_Object());

    auto object_ctor = static_cast<DexMethod*>(method::java_lang_Object_ctor());
    object_ctor->set_access(ACC_PUBLIC | ACC_CONSTRUCTOR);
    object_ctor->set_external();
    type_class(type::java_lang_Object())->add_method(object_ctor);

    // EnumFieldAnalyzer requires that this method exists
    method::java_lang_Enum_equals();
    DexField::make_field("Landroid/os/Build$VERSION;.SDK_INT:I");
    m_api_level_analyzer_state = ApiLevelAnalyzerState::get(min_sdk);
  }

  const int min_sdk = 42;
  ImmutableAttributeAnalyzerState m_immut_analyzer_state;
  ApiLevelAnalyzerState m_api_level_analyzer_state;
};

static DexStoresVector make_simple_stores(const Scope& scope) {
  auto store = DexStore("store");
  store.add_classes(scope);
  return DexStoresVector({store});
}

TEST_F(InterproceduralConstantPropagationTest, constantArgument) {
  // Let bar() be the only method calling baz(I)V, passing it a constant
  // argument. baz() should be optimized for that constant argument.

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto m1 = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
     (
      (load-param v0) ; the `this` argument
      (const v1 0)
      (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(I)V"
     (
      (load-param v0) ; the `this` argument
      (load-param v1)
      (if-eqz v1 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m2);

  auto cls = creator.create();
  scope.push_back(cls);
  InterproceduralConstantPropagationPass().run(make_simple_stores(scope));

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (const v1 0)
     (return-void)
    )
  )");

  EXPECT_CODE_EQ(m2->get_code(), expected_code2.get());
}

TEST_F(InterproceduralConstantPropagationTest, constantArgumentClass) {
  // Let bar() be the only method calling baz(...)V, passing it a constant
  // argument. baz() should be optimized for that constant argument,
  // which happens to be a type.

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto m1 = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
     (
      (load-param v0) ; the `this` argument
      (const-class "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1) "LFoo;.baz:(Ljava/lang/Class;)V")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(Ljava/lang/Class;)V"
     (
      (load-param v0) ; the `this` argument
      (load-param-object v1)
      (if-eqz v1 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m2);

  auto cls = creator.create();
  scope.push_back(cls);
  InterproceduralConstantPropagationPass().run(make_simple_stores(scope));

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param-object v1)
     (const-class "LFoo;")
     (move-result-pseudo-object v1)
     (const v0 0)
     (return-void)
    )
  )");

  EXPECT_CODE_EQ(m2->get_code(), expected_code2.get());
}

TEST_F(InterproceduralConstantPropagationTest, constantArgumentClassXStore) {
  // Let bar() be the only method calling baz(...)V, passing it a constant
  // argument. However, that argument is a type defined in a different storre
  // than baz, so the type reference should not be embedded into baz(). Still,
  // the knowledge that the type value is not zero will be used to optimize the
  // conditional branching in baz.

  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto m1 = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
     (
      (load-param v0) ; the `this` argument
      (const-class "LBar;")
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1) "LFoo;.baz:(Ljava/lang/Class;)V")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(Ljava/lang/Class;)V"
     (
      (load-param v0) ; the `this` argument
      (load-param-object v1)
      (if-eqz v1 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m2);

  auto cls = creator.create();
  auto store1 = DexStore("classes");
  store1.add_classes({cls});

  auto cls_ty2 = DexType::make_type("LBar;");
  ClassCreator creator2(cls_ty2);
  creator2.set_super(type::java_lang_Object());
  auto cls2 = creator2.create();
  auto store2 = DexStore("other_store");
  store2.add_classes({cls2});
  DexStoresVector stores({store1, store2});
  InterproceduralConstantPropagationPass().run(stores);

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param-object v1)
     (const v0 0)
     (return-void)
    )
  )");

  EXPECT_CODE_EQ(m2->get_code(), expected_code2.get());
}

TEST_F(InterproceduralConstantPropagationTest, constantTwoArgument) {
  // Let bar() be the only method calling baz(ILjava/lang/String;)V, passing it
  // a constant argument. baz() should be optimized for constant arguments.

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto m1 = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
     (
      (load-param v0) ; the `this` argument
      (const v1 0)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (invoke-direct (v0 v1 v2) "LFoo;.baz:(ILjava/lang/String;)V")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(ILjava/lang/String;)V"
     (
      (load-param v0) ; the `this` argument
      (load-param v1)
      (load-param-object v2)
      (if-eqz v1 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m2);

  auto cls = creator.create();
  scope.push_back(cls);
  InterproceduralConstantPropagationPass().run(make_simple_stores(scope));

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (load-param-object v2)
     (const v1 0)
     (const-string "hello")
     (move-result-pseudo-object v2)
     (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(m2->get_code()),
            assembler::to_s_expr(expected_code2.get()));
}

TEST_F(InterproceduralConstantPropagationTest, nonConstantArgument) {
  // Let there be two methods calling baz(I)V, passing it different arguments.
  // baz() cannot be optimized for a constant argument here.

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto m1 = assembler::method_from_string(R"(
    (method (public) "LFoo;.foo:()V"
     (
      (load-param v0) ; the `this` argument
      (const v1 0)
      (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
     (
      (load-param v0) ; the `this` argument
      (const v1 1)
      (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
      (return-void)
     )
    )
  )");
  m2->rstate.set_root();
  creator.add_method(m2);

  auto m3 = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(I)V"
     (
      (load-param v0) ; the `this` argument
      (load-param v1)
      (if-eqz v1 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m3);

  auto cls = creator.create();
  scope.push_back(cls);

  // m3's code should be unchanged since it cannot be optimized
  auto expected = assembler::to_s_expr(m3->get_code());
  InterproceduralConstantPropagationPass().run(make_simple_stores(scope));
  EXPECT_EQ(assembler::to_s_expr(m3->get_code()), expected);
}

TEST_F(InterproceduralConstantPropagationTest, argumentsGreaterThanZero) {
  // Let baz(I)V always be called with arguments > 0. baz() should be
  // optimized for that scenario.

  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto m1 = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()V"
     (
      (load-param v0) ; the `this` argument
      (const v1 1)
      (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar2:()V"
     (
      (load-param v0) ; the `this` argument
      (const v1 2)
      (invoke-direct (v0 v1) "LFoo;.baz:(I)V")
      (return-void)
     )
    )
  )");
  m2->rstate.set_root();
  creator.add_method(m2);

  auto m3 = assembler::method_from_string(R"(
    (method (private) "LFoo;.baz:(I)V"
     (
      (load-param v0) ; the `this` argument
      (load-param v1)
      (if-gtz v1 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m3);

  auto cls = creator.create();
  scope.push_back(cls);
  InterproceduralConstantPropagationPass().run(make_simple_stores(scope));

  auto expected_code3 = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (return-void)
    )
  )");

  EXPECT_CODE_EQ(m3->get_code(), expected_code3.get());
}

// We had a bug where an invoke instruction inside an unreachable block of code
// would cause the whole IPCP domain to be set to bottom. This test checks that
// we handle it correctly.
TEST_F(InterproceduralConstantPropagationTest, unreachableInvoke) {
  Scope scope;
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (const v0 0)
      (goto :skip)
      (invoke-static (v0) "LFoo;.qux:(I)V") ; this is unreachable
      (:skip)
      (invoke-static (v0) "LFoo;.baz:(I)V") ; this is reachable
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:(I)V"
     (
      (load-param v0)
      (return-void)
     )
    )
  )");
  creator.add_method(m2);

  auto m3 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.qux:(I)V"
      (
       (load-param v0)
       (return-void)
      )
    )
  )");
  creator.add_method(m3);

  auto cls = creator.create();
  scope.push_back(cls);

  auto cg = std::make_shared<call_graph::Graph>(call_graph::single_callee_graph(
      *method_override_graph::build_graph(scope), scope));
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });
  FixpointIterator fp_iter(
      std::move(cg),
      [](const DexMethod* method,
         const WholeProgramState&,
         const ArgumentDomain& args) {
        auto& code = *method->get_code();
        auto env = env_with_params(is_static(method), &code, args);
        auto intra_cp = std::make_unique<intraprocedural::FixpointIterator>(
            code.cfg(), ConstantPrimitiveAnalyzer());
        intra_cp->run(env);
        return intra_cp;
      });

  fp_iter.run({{CURRENT_PARTITION_LABEL, ArgumentDomain()}});

  // Check m2 is reachable, despite m3 being unreachable
  auto& graph = fp_iter.get_call_graph();

  signal(SIGABRT, crash_backtrace_handler);

  // Cache in variables, work around certain GCC bug. Must make a copy, as
  // intermediate domain is temporary.
  const auto res =
      fp_iter.get_entry_state_at(graph.node(m2)).get(CURRENT_PARTITION_LABEL);
  const auto exp = ArgumentDomain({{0, SignedConstantDomain(0)}});
  EXPECT_EQ(res, exp);
  EXPECT_TRUE(fp_iter.get_entry_state_at(graph.node(m3)).is_bottom());
}

struct RuntimeAssertTest : public InterproceduralConstantPropagationTest {
  DexMethodRef* m_fail_handler;

  RuntimeAssertTest() {
    m_config.max_heap_analysis_iterations = 1;
    m_config.create_runtime_asserts = true;
    m_config.runtime_assert.param_assert_fail_handler = DexMethod::make_method(
        "Lcom/facebook/redex/"
        "ConstantPropagationAssertHandler;.paramValueError:(I)V");
    m_config.runtime_assert.field_assert_fail_handler = DexMethod::make_method(
        "Lcom/facebook/redex/"
        "ConstantPropagationAssertHandler;.fieldValueError:(Ljava/lang/"
        "String;)V");
    m_config.runtime_assert.return_value_assert_fail_handler =
        DexMethod::make_method(
            "Lcom/facebook/redex/"
            "ConstantPropagationAssertHandler;.returnValueError:(Ljava/lang/"
            "String;)V");
  }

  InterproceduralConstantPropagationPass::Config m_config;
};

TEST_F(RuntimeAssertTest, RuntimeAssertEquality) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(I)V"
     (
      (load-param v0)
      (return-void)
     )
    )
  )");

  ConstantEnvironment env{{0, SignedConstantDomain(5)}};
  RuntimeAssertTransform rat(m_config.runtime_assert);
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  intraprocedural::FixpointIterator intra_cp(code->cfg(),
                                             ConstantPrimitiveAnalyzer());
  intra_cp.run(env);
  rat.apply(intra_cp, WholeProgramState(), method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (const v1 5)
      (if-eq v0 v1 :assertion-true)
      (const v2 0)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.paramValueError:(I)V")
      (:assertion-true)
      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(RuntimeAssertTest, RuntimeAssertSign) {
  using sign_domain::Interval;

  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(II)V"
     (
      (load-param v0)
      (load-param v1)
      (return-void)
     )
    )
  )");

  ConstantEnvironment env{{0, SignedConstantDomain(Interval::GEZ)},
                          {1, SignedConstantDomain(Interval::LTZ)}};
  RuntimeAssertTransform rat(m_config.runtime_assert);
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  intraprocedural::FixpointIterator intra_cp(code->cfg(),
                                             ConstantPrimitiveAnalyzer());
  intra_cp.run(env);
  rat.apply(intra_cp, WholeProgramState(), method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (if-gez v0 :assertion-true-1)
      (const v2 0)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.paramValueError:(I)V")
      (:assertion-true-1)
      (if-ltz v1 :assertion-true-2)
      (const v3 1)
      (invoke-static (v3) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.paramValueError:(I)V")
      (:assertion-true-2)
      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(RuntimeAssertTest, RuntimeAssertCheckIntOnly) {
  using sign_domain::Interval;

  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(JI)V"
     (
       (load-param v0) ; long -- we don't handle this yet
       (load-param v1) ; int
       (return-void)
     )
    )
  )");

  ConstantEnvironment env{{0, SignedConstantDomain(Interval::GEZ)},
                          {1, SignedConstantDomain(Interval::LTZ)}};
  RuntimeAssertTransform rat(m_config.runtime_assert);
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  intraprocedural::FixpointIterator intra_cp(code->cfg(),
                                             ConstantPrimitiveAnalyzer());
  intra_cp.run(env);
  rat.apply(intra_cp, WholeProgramState(), method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (if-ltz v1 :assertion-true-1)
      (const v2 1)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.paramValueError:(I)V")
      (:assertion-true-1)
      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(RuntimeAssertTest, RuntimeAssertCheckVirtualMethod) {
  using sign_domain::Interval;

  auto method = assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:(I)V"
     (
      (load-param v0) ; `this` argument
      (load-param v1)
      (return-void)
     )
    )
  )");

  ConstantEnvironment env{{1, SignedConstantDomain(Interval::LTZ)}};
  RuntimeAssertTransform rat(m_config.runtime_assert);
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  intraprocedural::FixpointIterator intra_cp(code->cfg(),
                                             ConstantPrimitiveAnalyzer());
  intra_cp.run(env);
  rat.apply(intra_cp, WholeProgramState(), method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0) ; `this` argument
      (load-param v1)
      (if-ltz v1 :assertion-true-1)
      (const v2 0)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.paramValueError:(I)V")
      (:assertion-true-1)
      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(RuntimeAssertTest, RuntimeAssertField) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  // We must create a field def and attach it to the DexClass instance (instead
  // of just creating an unattached field ref) so that when IPC calls
  // resolve_field() on Foo.qux, they will find it and treat it as a known field
  auto field = DexField::make_field("LFoo;.qux:I")
                   ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                   std::unique_ptr<DexEncodedValue>(
                                       new DexEncodedValueBit(DEVT_INT, 1)));
  creator.add_field(field);

  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (sget "LFoo;.qux:I")
      (move-result-pseudo v0)
      (return-void)
     )
    )
  )");
  creator.add_method(method);

  Scope scope{creator.create()};
  InterproceduralConstantPropagationPass(m_config).run(
      make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget "LFoo;.qux:I")
      (move-result-pseudo v0)
      (const v1 1)
      (if-eq v0 v1 :ok)

      (const-string "qux")
      (move-result-pseudo-object v2)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.fieldValueError:(Ljava/lang/String;)V")

      (:ok)
      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(RuntimeAssertTest, RuntimeAssertConstantReturnValue) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (invoke-static () "LFoo;.constantReturnValue:()I")
      (move-result v0)
      (return-void)
     )
    )
  )");
  creator.add_method(method);

  auto constant_return_method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.constantReturnValue:()I"
     (
      (const v0 1)
      (return v0)
     )
    )
  )");
  creator.add_method(constant_return_method);

  Scope scope{creator.create()};
  InterproceduralConstantPropagationPass(m_config).run(
      make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (invoke-static () "LFoo;.constantReturnValue:()I")
      (move-result v0)
      (const v1 1)
      (if-eq v0 v1 :ok)

      (const-string "constantReturnValue")
      (move-result-pseudo-object v2)
      (invoke-static (v2) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.returnValueError:(Ljava/lang/String;)V")

      (:ok)
      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(RuntimeAssertTest, RuntimeAssertNeverReturnsVoid) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (invoke-static () "LFoo;.neverReturns:()V")
      (return-void)
     )
    )
  )");
  creator.add_method(method);

  auto never_returns = assembler::method_from_string(R"(
    (method (public static) "LFoo;.neverReturns:()V"
     (
       (:loop)
       (goto :loop)
     )
    )
  )");
  creator.add_method(never_returns);

  Scope scope{creator.create()};
  InterproceduralConstantPropagationPass(m_config).run(
      make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (invoke-static () "LFoo;.neverReturns:()V")

      (const-string "neverReturns")
      (move-result-pseudo-object v0)
      (invoke-static (v0) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.returnValueError:(Ljava/lang/String;)V")

      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(RuntimeAssertTest, RuntimeAssertNeverReturnsConstant) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (invoke-static () "LFoo;.neverReturns:()I")
      (move-result v0)
      (return-void)
     )
    )
  )");
  creator.add_method(method);

  auto never_returns = assembler::method_from_string(R"(
    (method (public static) "LFoo;.neverReturns:()I"
     (
       (:loop)
       (goto :loop)
     )
    )
  )");
  creator.add_method(never_returns);

  Scope scope{creator.create()};
  InterproceduralConstantPropagationPass(m_config).run(
      make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (invoke-static () "LFoo;.neverReturns:()I")
      (move-result v0)

      (const-string "neverReturns")
      (move-result-pseudo-object v1)
      (invoke-static (v1) "Lcom/facebook/redex/ConstantPropagationAssertHandler;.returnValueError:(Ljava/lang/String;)V")

      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest, constantField) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto field = DexField::make_field("LFoo;.qux:I")
                   ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                   std::unique_ptr<DexEncodedValue>(
                                       new DexEncodedValueBit(DEVT_INT, 1)));
  creator.add_field(field);

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (const v0 1)
      (sput v0 "LFoo;.qux:I")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root(); // Make this an entry point
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:()V"
     (
      (sget "LFoo;.qux:I")
      (move-result-pseudo v0)
      (if-nez v0 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  m2->rstate.set_root(); // Make this an entry point
  creator.add_method(m2);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_code2 = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (return-void)
    )
  )");

  EXPECT_CODE_EQ(m2->get_code(), expected_code2.get());
}

TEST_F(InterproceduralConstantPropagationTest, nonConstantField) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto field = DexField::make_field("LFoo;.qux:I")
                   ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                   std::unique_ptr<DexEncodedValue>(
                                       new DexEncodedValueBit(DEVT_INT, 1)));
  creator.add_field(field);

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (const v0 0) ; this differs from the original encoded value of Foo.qux
      (sput v0 "LFoo;.qux:I")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root(); // Make this an entry point
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:()V"
     (
      (sget "LFoo;.qux:I")
      (move-result-pseudo v0)
      (if-nez v0 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  m2->rstate.set_root(); // Make this an entry point
  creator.add_method(m2);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  auto expected = assembler::to_s_expr(m2->get_code());

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  EXPECT_EQ(assembler::to_s_expr(m2->get_code()), expected);
}

TEST_F(InterproceduralConstantPropagationTest, nonConstantFieldDueToKeep) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto field = DexField::make_field("LFoo;.qux:I")
                   ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                   std::unique_ptr<DexEncodedValue>(
                                       new DexEncodedValueBit(DEVT_INT, 1)));
  creator.add_field(field);

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (const v0 1)
      (sput v0 "LFoo;.qux:I")
      (return-void)
     )
    )
  )");
  m1->rstate.set_root(); // Make this an entry point
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:()V"
     (
      (sget "LFoo;.qux:I")
      (move-result-pseudo v0)
      (if-nez v0 :label)
      (const v0 0)
      (:label)
      (return-void)
     )
    )
  )");
  m2->rstate.set_root(); // Make this an entry point
  creator.add_method(m2);

  // Mark Foo.qux as a -keep field -- meaning we cannot determine if its value
  // is truly constant just by looking at Dex bytecode
  static_cast<DexField*>(DexField::get_field("LFoo;.qux:I"))->rstate.set_root();
  auto expected = assembler::to_s_expr(m2->get_code());

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  EXPECT_EQ(assembler::to_s_expr(m2->get_code()), expected);
}

TEST_F(InterproceduralConstantPropagationTest, constantFieldAfterClinit) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto field_qux =
      DexField::make_field("LFoo;.qux:I")
          ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                          std::unique_ptr<DexEncodedValue>(
                              new DexEncodedValueBit(DEVT_INT, 1)));
  creator.add_field(field_qux);

  auto field_corge = DexField::make_field("LFoo;.corge:I")
                         ->make_concrete(ACC_PUBLIC | ACC_STATIC);
  creator.add_field(field_corge);

  auto clinit = assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (sget "LFoo;.qux:I")     ; Foo.qux is the constant 0 outside this clinit,
      (move-result-pseudo v0)  ; but we should check that we don't overwrite
      (sput v0 "LFoo;.corge:I") ; its initial encoded value while transforming
                               ; the clinit. I.e. this sget should be converted
                               ; to "const v0 1", not "const v0 0".

      (const v0 0) ; this differs from the original encoded value of Foo.qux,
                   ; but will be the only field value visible to other methods
      (sput v0 "LFoo;.qux:I")
      (return-void)
     )
    )
  )");
  clinit->rstate.set_root(); // Make this an entry point
  creator.add_method(clinit);

  auto m = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:()V"
     (
      (sget "LFoo;.qux:I")
      (move-result-pseudo v0) ; this is always zero due to <clinit>
      (if-nez v0 :label)
      (const v0 1)
      (:label)
      (return-void)
     )
    )
  )");
  m->rstate.set_root(); // Make this an entry point
  creator.add_method(m);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 2;

  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();
  EXPECT_EQ(wps.get_field_value(field_qux), SignedConstantDomain(0));
  EXPECT_EQ(wps.get_field_value(field_corge), SignedConstantDomain(1));

  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_clinit_code = assembler::ircode_from_string(R"(
     (
      (const v0 1)
      (sput v0 "LFoo;.corge:I") ; these field writes will be removed by RMUF
      (const v0 0)
      (sput v0 "LFoo;.qux:I")
      (return-void)
     )
  )");

  EXPECT_CODE_EQ(clinit->get_code(), expected_clinit_code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v0 1)
     (return-void)
    )
  )");

  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest,
       nonConstantFieldDueToInvokeInClinit) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());
  auto field_qux =
      DexField::make_field("LFoo;.qux:I")
          ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                          std::unique_ptr<DexEncodedValue>(
                              new DexEncodedValueBit(DEVT_INT, 0)));
  creator.add_field(field_qux);

  auto clinit = assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (invoke-static () "LFoo;.initQux:()V")
      (return-void)
     )
    )
  )");
  clinit->rstate.set_root(); // Make this an entry point
  creator.add_method(clinit);

  auto init_qux = assembler::method_from_string(R"(
    (method (public static) "LFoo;.initQux:()V"
     (
      (const v0 1) ; this differs from the original encoded value of Foo.qux
      (sput v0 "LFoo;.qux:I")
      (return-void)
     )
    )
  )");
  creator.add_method(init_qux);

  auto m = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:()V"
     (
      (sget "LFoo;.qux:I")
      (move-result-pseudo v0)
      (if-nez v0 :label)
      (const v0 1)
      (:label)
      (return-void)
     )
    )
  )");
  m->rstate.set_root(); // Make this an entry point
  creator.add_method(m);

  // We expect Foo.baz() to be unchanged since Foo.qux is not a constant
  auto expected = assembler::to_s_expr(m->get_code());

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;

  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();
  EXPECT_EQ(wps.get_field_value(field_qux), ConstantValue::top());

  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));
  EXPECT_EQ(assembler::to_s_expr(m->get_code()), expected);
}

TEST_F(InterproceduralConstantPropagationTest, constantReturnValue) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (invoke-static () "LFoo;.constantReturnValue:()I")
      (move-result v0)
      (if-eqz v0 :label)
      (const v0 1)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.constantReturnValue:()I"
     (
      (const v0 0)
      (return v0)
     )
    )
  )");
  creator.add_method(m2);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (invoke-static () "LFoo;.constantReturnValue:()I")
     (move-result v0)
     (return-void)
    )
  )");

  EXPECT_CODE_EQ(m1->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest, VirtualMethodReturnValue) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());
  creator.set_access(creator.get_access() | ACC_NATIVE);

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(LFoo;)V"
     (
      (load-param-object v0)
      (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
      (move-result v0) ; Constant value since this virtualMethod is not overridden
      (if-eqz v0 :label)
      (const v0 1)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public) "LFoo;.virtualMethod:()I"
     (
      (const v0 0)
      (return v0)
     )
    )
  )");
  creator.add_method(m2);
  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
     (move-result v0)
     (return-void)
    )
  )");

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));
  EXPECT_CODE_EQ(m1->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest, RootVirtualMethodReturnValue) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());
  creator.set_access(creator.get_access() | ACC_NATIVE);

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(LFoo;)V"
     (
      (load-param-object v0)
      (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
      (move-result v0) ; Not propagating value because virtualMethod is root
      (if-eqz v0 :label)
      (const v0 1)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public) "LFoo;.virtualMethod:()I"
     (
      (const v0 0)
      (return v0)
     )
    )
  )");
  m2->rstate.set_root();
  creator.add_method(m2);
  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
     (move-result v0)
     (if-eqz v0 :label)
     (const v0 1)
     (:label)
     (return-void)
    )
  )");

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));
  EXPECT_CODE_EQ(m1->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest, NativeImplementReturnValue) {
  auto cls1_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls1_ty);
  creator.set_super(type::java_lang_Object());
  creator.set_access(creator.get_access() | ACC_NATIVE);

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(LFoo;)V"
     (
      (load-param-object v0)
      (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
      (move-result v0) ; Not propagating value because virtualMethod is root
      (if-eqz v0 :label)
      (const v0 1)
      (:label)
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);
  auto cls1 = creator.create();
  auto void_int =
      DexProto::make_proto(type::_int(), DexTypeList::make_type_list({}));
  auto method_base = static_cast<DexMethod*>(DexMethod::make_method(
      cls1_ty, DexString::make_string("virtualMethod"), void_int));
  method_base->make_concrete(
      ACC_PUBLIC | ACC_ABSTRACT, std::unique_ptr<IRCode>(nullptr), true);
  cls1->add_method(method_base);

  auto cls2_ty = DexType::make_type("LBoo;");
  ClassCreator creator2(cls2_ty);
  creator2.set_super(cls1_ty);
  creator2.set_access(creator2.get_access() | ACC_NATIVE);
  auto m2 = assembler::method_from_string(R"(
    (method (public) "LBoo;.virtualMethod:()I"
     (
      (const v0 0)
      (return v0)
     )
    )
  )");
  creator2.add_method(m2);
  auto cls2 = creator2.create();

  auto cls3_ty = DexType::make_type("LBar;");
  ClassCreator creator3(cls3_ty);
  creator3.set_super(cls1_ty);
  creator3.set_access(creator3.get_access() | ACC_NATIVE);
  DexMethodRef* m3_ref = DexMethod::make_method("LBar;.virtualMethod:()I");
  auto m3 = m3_ref->make_concrete(ACC_PUBLIC | ACC_NATIVE, true);
  creator3.add_method(m3);
  auto cls3 = creator3.create();

  Scope scope{cls1, cls2, cls3};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
     (move-result v0)
     (if-eqz v0 :label)
     (const v0 1)
     (:label)
     (return-void)
    )
  )");

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  config.use_multiple_callee_callgraph = true;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));
  EXPECT_CODE_EQ(m1->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest,
       NativeInterfaceImplementReturnValue) {
  auto cls1_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls1_ty);
  creator.set_super(type::java_lang_Object());
  creator.set_access(creator.get_access() | ACC_NATIVE | ACC_INTERFACE);

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(LFoo;)V"
     (
      (load-param-object v0)
      (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
      (move-result v0) ; Not propagating value because virtualMethod is root
      (if-eqz v0 :label)
      (const v0 1)
      (:label)
      (return-void)
     )
    )
  )");
  m1->rstate.set_root();
  creator.add_method(m1);
  auto cls1 = creator.create();
  auto void_int =
      DexProto::make_proto(type::_int(), DexTypeList::make_type_list({}));
  auto method_base = static_cast<DexMethod*>(DexMethod::make_method(
      cls1_ty, DexString::make_string("virtualMethod"), void_int));
  method_base->make_concrete(
      ACC_PUBLIC | ACC_INTERFACE, std::unique_ptr<IRCode>(nullptr), true);
  cls1->add_method(method_base);

  auto cls2_ty = DexType::make_type("LBoo;");
  ClassCreator creator2(cls2_ty);
  creator2.set_super(type::java_lang_Object());
  creator2.add_interface(cls1_ty);
  creator2.set_access(creator2.get_access() | ACC_NATIVE);
  auto m2 = assembler::method_from_string(R"(
    (method (public) "LBoo;.virtualMethod:()I"
     (
      (const v0 0)
      (return v0)
     )
    )
  )");
  creator2.add_method(m2);
  auto cls2 = creator2.create();

  auto cls3_ty = DexType::make_type("LBar;");
  ClassCreator creator3(cls3_ty);
  creator3.set_super(type::java_lang_Object());
  creator3.add_interface(cls1_ty);
  creator3.set_access(creator3.get_access() | ACC_NATIVE);
  DexMethodRef* m3_ref = DexMethod::make_method("LBar;.virtualMethod:()I");
  auto m3 = m3_ref->make_concrete(ACC_PUBLIC | ACC_NATIVE, true);
  creator3.add_method(m3);
  auto cls3 = creator3.create();

  Scope scope{cls1, cls2, cls3};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
     (move-result v0)
     (if-eqz v0 :label)
     (const v0 1)
     (:label)
     (return-void)
    )
  )");

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  config.use_multiple_callee_callgraph = true;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));
  EXPECT_CODE_EQ(m1->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest,
       OverrideVirtualMethodReturnValue) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());
  creator.set_access(creator.get_access() | ACC_NATIVE);

  auto cls_child_ty = DexType::make_type("LBoo;");
  ClassCreator child_creator(cls_child_ty);
  child_creator.set_super(cls_ty);
  child_creator.set_access(child_creator.get_access() | ACC_NATIVE);

  auto m1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(LFoo;)V"
     (
      (load-param-object v0)
      (invoke-virtual (v0) "LFoo;.virtualMethod:()I")
      (move-result v0) ; not a constant value since virtualMethod can be overridden
      (if-eqz v0 :label)
      (const v0 1)
      (:label)
      (return-void)
     )
    )
  )");
  creator.add_method(m1);

  auto m2 = assembler::method_from_string(R"(
    (method (public) "LFoo;.virtualMethod:()I"
     (
      (const v0 0)
      (return v0)
     )
    )
  )");
  creator.add_method(m2);

  auto child_m3 = assembler::method_from_string(R"(
    (method (public) "LBoo;.virtualMethod:()I"
     (
      (const v0 0)
      (return v0)
     )
    )
  )");
  child_creator.add_method(child_m3);
  DexStore store("classes");
  store.add_classes({creator.create()});
  store.add_classes({child_creator.create()});
  std::vector<DexStore> stores;
  stores.emplace_back(std::move(store));
  auto scope = build_class_scope(stores);
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  auto expected = assembler::to_s_expr(m1->get_code());

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));
  EXPECT_EQ(assembler::to_s_expr(m1->get_code()), expected);
}

TEST_F(InterproceduralConstantPropagationTest, neverReturns) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:(I)V"
     (
      (load-param v0)
      (if-eqz v0 :if-true-1)

      (invoke-static () "LFoo;.neverReturns:()V")
      (const v1 0) ; this never executes

      (:if-true-1)
      (const v1 1) ; this is the only instruction assigning to v1

      (const v2 1)
      (if-eq v1 v2 :if-true-2) ; this should always be true
      (const v3 2)
      (:if-true-2)
      (return-void)
     )
    )
  )");
  creator.add_method(method);
  method->rstate.set_root(); // Make this an entry point

  auto never_returns = assembler::method_from_string(R"(
    (method (public static) "LFoo;.neverReturns:()V"
     (
       (:loop)
       (goto :loop)
     )
    )
  )");
  creator.add_method(never_returns);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (if-eqz v0 :if-true-1)

     (invoke-static () "LFoo;.neverReturns:()V")
     (const v1 0)

     (:if-true-1)
     (const v1 1)

     (const v2 1)
     (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest, whiteBoxReturnValues) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto returns_void = assembler::method_from_string(R"(
    (method (public static) "LFoo;.returnsVoid:()V"
     (
      (return-void)
     )
    )
  )");
  creator.add_method(returns_void);

  auto never_returns = assembler::method_from_string(R"(
    (method (public static) "LFoo;.neverReturns:()V"
     (
       (:loop)
       (goto :loop)
     )
    )
  )");
  creator.add_method(never_returns);

  auto returns_constant = assembler::method_from_string(R"(
    (method (public static) "LFoo;.returnsConstant:()I"
     (
      (const v0 1)
      (return v0)
     )
    )
  )");
  creator.add_method(returns_constant);

  auto no_code = DexMethod::make_method("LFoo;.no_code:()V")
                     ->make_concrete(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE, true);
  creator.add_method(no_code);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();

  // Make sure we mark methods that have a reachable return-void statement as
  // "returning" Top.
  // And for a method that has no implementation in dex we also want its
  // return value be Top but not Bottom.
  EXPECT_EQ(wps.get_return_value(returns_void), SignedConstantDomain::top());
  EXPECT_EQ(wps.get_return_value(no_code), SignedConstantDomain::top());
  EXPECT_EQ(wps.get_return_value(never_returns),
            SignedConstantDomain::bottom());
  EXPECT_EQ(wps.get_return_value(returns_constant), SignedConstantDomain(1));
}

TEST_F(InterproceduralConstantPropagationTest, min_sdk) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto returns_min_sdk = assembler::method_from_string(R"(
    (method (public static) "LFoo;.returnsConstant:()I"
     (
      (sget "Landroid/os/Build$VERSION;.SDK_INT:I")
      (move-result-pseudo v0)
      (return v0)
     )
    )
  )");
  creator.add_method(returns_min_sdk);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 1;
  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();

  // Make sure we mark methods that have a reachable return-void statement as
  // "returning" Top.
  // And for a method that has no implementation in dex we also want its
  // return value be Top but not Bottom.
  EXPECT_EQ(wps.get_return_value(returns_min_sdk),
            SignedConstantDomain(min_sdk, std::numeric_limits<int32_t>::max()));
}

TEST_F(InterproceduralConstantPropagationTest, ghost_edges) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto does_not_return = assembler::method_from_string(R"(
    (method (public static) "LFoo;.doesNotTerminate:()I"
     (
      (load-param v0)
      (if-eqz v0 :loop2)

      (:loop1)
      (const v0 0)
      (if-eqz v0 :loop1)
      (goto :loop1)

      (:loop2)
      (const v0 0)
      (if-eqz v0 :loop2)
      (goto :loop2)
     )
    )
  )");
  creator.add_method(does_not_return);

  Scope scope{creator.create()};

  // Check that cfg will indeed have ghost edges...
  auto code = does_not_return->get_code();
  code->build_cfg(/* editable */ true);
  code->cfg().calculate_exit_block();
  auto exit_block = does_not_return->get_code()->cfg().exit_block();
  EXPECT_NE(exit_block, nullptr);
  EXPECT_EQ(exit_block->preds().size(), 2);
  EXPECT_EQ(exit_block->preds().front()->type(), cfg::EDGE_GHOST);
  code->clear_cfg();

  InterproceduralConstantPropagationPass().run(make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :loop2)

      (:loop1)
      (const v0 0)
      (goto :loop1)

      (:loop2)
      (const v0 0)
      (goto :loop2)
    )
  )");

  EXPECT_CODE_EQ(code, expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest,
       nezConstantFieldAfterInit_simple) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto field_f = DexField::make_field("LFoo;.f:I")->make_concrete(ACC_PUBLIC);
  creator.add_field(field_f);

  auto init = assembler::method_from_string(R"(
    (method (public constructor) "LFoo;.<init>:()V"
     (
      (load-param-object v0)
      (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
      (const v1 42)
      (iput v1 v0 "LFoo;.f:I")
      (return-void)
     )
    )
  )");
  init->rstate.set_root(); // Make this an entry point
  creator.add_method(init);

  auto m = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:(LFoo;)I"
     (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (return v0)
     )
    )
  )");
  m->rstate.set_root(); // Make this an entry point
  creator.add_method(m);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 2;

  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();
  // as the field is definitely-assigned, 0 was not added to the numeric
  // interval domain
  EXPECT_EQ(wps.get_field_value(field_f), SignedConstantDomain(42));

  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (const v0 42)
      (return v0)
    )
  )");

  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest,
       nezConstantFieldAfterInit_branching) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto field_f = DexField::make_field("LFoo;.f:I")->make_concrete(ACC_PUBLIC);
  creator.add_field(field_f);

  auto init = assembler::method_from_string(R"(
    (method (public constructor) "LFoo;.<init>:(Z)V"
     (
      (load-param-object v0)
      (load-param v2)
      (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
      (if-eqz v2 :second)
      (const v1 42) ; feasible
      (iput v1 v0 "LFoo;.f:I")
      (return-void)
      (:second)
      (const v1 23) ; feasible
      (iput v1 v0 "LFoo;.f:I")
      (return-void)
     )
    )
  )");
  init->rstate.set_root(); // Make this an entry point
  creator.add_method(init);

  auto m = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:(LFoo;)I"
     (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (const v1 300)
      (if-gtz v0 :skip)
      (const v1 400)
      (:skip)
      (return v1)
     )
    )
  )");
  m->rstate.set_root(); // Make this an entry point
  creator.add_method(m);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 2;

  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();
  // as the field is definitely-assigned, even with the branching in the
  // constructor, 0 was not added to the numeric interval domain
  EXPECT_EQ(wps.get_field_value(field_f), SignedConstantDomain(23, 42));

  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (const v1 300)
      (return v1)
    )
  )");

  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest,
       constantFieldAfterInit_this_escaped) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto field_f = DexField::make_field("LFoo;.f:I")->make_concrete(ACC_PUBLIC);
  creator.add_field(field_f);

  auto init = assembler::method_from_string(R"(
    (method (public constructor) "LFoo;.<init>:()V"
     (
      (load-param-object v0)
      (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
      (sput-object v0 "LFoo;.some_global_field:LFoo;") ; 'this' escapes here
      (const v1 42)
      (iput v1 v0 "LFoo;.f:I")
      (return-void)
     )
    )
  )");
  init->rstate.set_root(); // Make this an entry point
  creator.add_method(init);

  auto m = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:(LFoo;)I"
     (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (return v0)
     )
    )
  )");
  m->rstate.set_root(); // Make this an entry point
  creator.add_method(m);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 2;

  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();
  // 0 is included in the numeric interval as 'this' escaped before the
  // assignment
  EXPECT_EQ(wps.get_field_value(field_f), SignedConstantDomain(0, 42));

  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (return v0)
    )
  )");

  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest,
       constantFieldAfterInit_nontrivial_external_base_ctor) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Throwable());

  auto field_f = DexField::make_field("LFoo;.f:I")->make_concrete(ACC_PUBLIC);
  creator.add_field(field_f);

  auto init = assembler::method_from_string(R"(
    (method (public constructor) "LFoo;.<init>:()V"
     (
      (load-param-object v0)
      (invoke-direct (v0) "Ljava/lang/Throwable;.<init>:()V") ; 'this' escapes here
      (const v1 42)
      (iput v1 v0 "LFoo;.f:I")
      (return-void)
     )
    )
  )");
  init->rstate.set_root(); // Make this an entry point
  creator.add_method(init);

  auto m = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:(LFoo;)I"
     (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (return v0)
     )
    )
  )");
  m->rstate.set_root(); // Make this an entry point
  creator.add_method(m);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 2;

  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();
  // 0 is included in the numeric interval as 'this' escaped before the
  // assignment
  EXPECT_EQ(wps.get_field_value(field_f), SignedConstantDomain(0, 42));

  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (return v0)
    )
  )");

  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}

TEST_F(InterproceduralConstantPropagationTest,
       constantFieldAfterInit_read_before_write) {
  auto cls_ty = DexType::make_type("LFoo;");
  ClassCreator creator(cls_ty);
  creator.set_super(type::java_lang_Object());

  auto field_f = DexField::make_field("LFoo;.f:I")->make_concrete(ACC_PUBLIC);
  creator.add_field(field_f);

  auto init = assembler::method_from_string(R"(
    (method (public constructor) "LFoo;.<init>:()V"
     (
      (load-param-object v0)
      (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
      (iget v0 "LFoo;.f:I") ; read before...
      (move-result-pseudo v1)
      (const v1 42)
      (iput v1 v0 "LFoo;.f:I") ; ...write
      (return-void)
     )
    )
  )");
  init->rstate.set_root(); // Make this an entry point
  creator.add_method(init);

  auto m = assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:(LFoo;)I"
     (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (return v0)
     )
    )
  )");
  m->rstate.set_root(); // Make this an entry point
  creator.add_method(m);

  Scope scope{creator.create()};
  walk::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });

  InterproceduralConstantPropagationPass::Config config;
  config.max_heap_analysis_iterations = 2;

  auto fp_iter = InterproceduralConstantPropagationPass(config).analyze(
      scope, &m_immut_analyzer_state, &m_api_level_analyzer_state);
  auto& wps = fp_iter->get_whole_program_state();
  // 0 is included in the numeric interval as the field was read before written
  EXPECT_EQ(wps.get_field_value(field_f), SignedConstantDomain(0, 42));

  InterproceduralConstantPropagationPass(config).run(make_simple_stores(scope));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (iget v0 "LFoo;.f:I")
      (move-result-pseudo v0)
      (return v0)
    )
  )");

  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}
