/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "RemoveUninstantiablesPass.h"
#include "ScopeHelper.h"
#include "VirtualScope.h"

namespace {

struct RemoveUninstantiablesTest : public RedexTest {
  RemoveUninstantiablesTest() {
    always_assert(type_class(type::java_lang_Object()) == nullptr);
    always_assert(type_class(type::java_lang_Void()) == nullptr);
    ClassCreator cc_object(type::java_lang_Object());
    cc_object.set_access(ACC_PUBLIC);
    cc_object.create();
    ClassCreator cc_void(type::java_lang_Void());
    cc_void.set_access(ACC_PUBLIC | ACC_ABSTRACT);
    cc_void.set_super(type::java_lang_Object());
    cc_void.create();
  }
};

std::unordered_set<DexType*> compute_uninstantiable_types() {
  Scope scope;
  g_redex->walk_type_class([&](const DexType*, const DexClass* cls) {
    scope.push_back(const_cast<DexClass*>(cls));
  });
  scope.push_back(type_class(type::java_lang_Void()));
  return RemoveUninstantiablesPass::compute_scoped_uninstantiable_types(scope);
}

RemoveUninstantiablesPass::Stats replace_uninstantiable_refs(
    cfg::ControlFlowGraph& cfg) {
  return RemoveUninstantiablesPass::replace_uninstantiable_refs(
      compute_uninstantiable_types(), cfg);
}

RemoveUninstantiablesPass::Stats replace_all_with_throw(
    cfg::ControlFlowGraph& cfg) {
  return RemoveUninstantiablesPass::replace_all_with_throw(cfg);
}

/// Expect \c RemoveUninstantiablesPass to convert \p ACTUAL into \p EXPECTED
/// where both parameters are strings containing IRCode in s-expression form.
/// Increments the stats returned from performing \p OPERATION to the variable
/// with identifier \p STATS.
#define EXPECT_CHANGE(OPERATION, STATS, ACTUAL, EXPECTED)             \
  do {                                                                \
    auto actual_ir = assembler::ircode_from_string(ACTUAL);           \
    const auto expected_ir = assembler::ircode_from_string(EXPECTED); \
                                                                      \
    actual_ir->build_cfg();                                           \
    STATS += OPERATION(actual_ir->cfg());                             \
    actual_ir->clear_cfg();                                           \
                                                                      \
    EXPECT_CODE_EQ(expected_ir.get(), actual_ir.get());               \
  } while (0)

/// Expect method with full signature \p SIGNATURE to exist, and have a
/// body corresponding to \p EXPECTED, a string containing IRCode in
/// s-expression form.
#define EXPECT_METHOD(SIGNATURE, EXPECTED)                            \
  do {                                                                \
    std::string signature = (SIGNATURE);                              \
    auto method = DexMethod::get_method(signature);                   \
    EXPECT_NE(nullptr, method) << "Method not found: " << signature;  \
                                                                      \
    auto actual_ir = method->as_def()->get_code();                    \
    const auto expected_ir = assembler::ircode_from_string(EXPECTED); \
    EXPECT_CODE_EQ(expected_ir.get(), actual_ir);                     \
  } while (0)

/// Expect method with full signature \p SIGNATURE to not exist.
#define EXPECT_NO_METHOD_DEF(SIGNATURE)             \
  do {                                              \
    std::string signature = (SIGNATURE);            \
    auto method = DexMethod::get_method(signature); \
    EXPECT_TRUE(!method || !method->is_def());      \
                                                    \
  } while (0)

/// Expect method with full signature \p SIGNATURE to exist, and be
/// abstract.
#define EXPECT_ABSTRACT_METHOD(SIGNATURE)                            \
  do {                                                               \
    std::string signature = (SIGNATURE);                             \
    auto method = DexMethod::get_method(signature);                  \
    EXPECT_NE(nullptr, method) << "Method not found: " << signature; \
    EXPECT_TRUE(is_abstract(method->as_def()));                      \
  } while (0)

/// Register a new class with \p name, and methods \p methods, given in
/// s-expression form.
template <typename... Methods>
DexClass* def_class(const char* name, Methods... methods) {
  return assembler::class_with_methods(
      name,
      {
          assembler::method_from_string(methods)...,
      });
}

const char* const Bar_init = R"(
(method (private) "LBar;.<init>:()V"
  ((load-param-object v0)
   (return-void))
))";

const char* const Bar_baz = R"(
(method (public) "LBar;.baz:()V"
  ((load-param-object v0)
   (return-void))
))";

const char* const Bar_qux = R"(
(method (public) "LBar;.qux:()I"
  ((load-param-object v0) ; this
   (iget-object v0 "LBar;.mFoo:LFoo;")
   (move-result-pseudo-object v1)
   (iput-object v1 v0 "LBar;.mFoo:LFoo;")
   (if-eqz v1 :else)
   (invoke-virtual (v1) "LFoo;.qux:()LFoo;")
   (move-result-object v2)
   (instance-of v2 "LFoo;")
   (move-result-pseudo v3)
   (return v3)
   (:else)
   (iget-object v1 "LFoo;.mBar:LBar;")
   (move-result-pseudo-object v3)
   (const v4 0)
   (return v4))
))";

const char* const BarBar_init = R"(
(method (private) "LBarBar;.<init>:()V"
  ((load-param-object v0)
   (invoke-direct (v0) "LBar;.<init>:()V")
   (return-void))
))";

const char* const BarBar_baz = R"(
(method (public) "LBarBar;.baz:()V"
  ((load-param-object v0)
   (new-instance "LBarBar;")
   (move-result-pseudo-object v1)
   (return-void))
))";

const char* const Foo_baz = R"(
(method (public) "LFoo;.baz:()V"
  ((load-param-object v0)
   (return-void))
))";

const char* const Foo_qux = R"(
(method (public) "LFoo;.qux:()LFoo;"
  ((load-param-object v0)
   (return-object v0))
))";

const char* const Foo_fox = R"(
(method (private) "LFoo;.fox:()LFoo;"
  ((load-param-object v0)
   (return-object v0))
))";

const char* const FooBar_baz = R"(
(method (public) "LFooBar;.baz:()V"
  ((load-param-object v0)
   (return-void))
))";

TEST_F(RemoveUninstantiablesTest, InstanceOf) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  ASSERT_TRUE(type::is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(type::is_uninstantiable_class(DexType::get_type("LBar;")));

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (instance-of v0 "LFoo;")
                  (move-result-pseudo v1)
                  (instance-of v0 "LBar;")
                  (move-result-pseudo v1)
                ))",
                /* EXPECTED */ R"((
                  (const v1 0)
                  (instance-of v0 "LBar;")
                  (move-result-pseudo v1)
                ))");

  EXPECT_EQ(1, stats.instance_ofs);
}

TEST_F(RemoveUninstantiablesTest, InstanceOfUnimplementedInterface) {
  auto cls = def_class("LFoo;");
  cls->set_access(cls->get_access() | ACC_INTERFACE | ACC_ABSTRACT);

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (instance-of v0 "LFoo;")
                  (move-result-pseudo v1)
                ))",
                /* EXPECTED */ R"((
                  (const v1 0)
                ))");

  EXPECT_EQ(1, stats.instance_ofs);
}

TEST_F(RemoveUninstantiablesTest, Invoke) {
  def_class("LFoo;", Foo_baz, Foo_qux);
  def_class("LBar;", Bar_init, Bar_baz);

  ASSERT_TRUE(type::is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(type::is_uninstantiable_class(DexType::get_type("LBar;")));

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (invoke-virtual (v0) "LFoo;.qux:()LFoo;")
                  (move-result-object v1)
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (const v2 0)
                  (throw v2)
                ))");
  EXPECT_EQ(1, stats.invokes);

  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (invoke-virtual (v0) "LFoo;.baz:()V")
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (const v1 0)
                  (throw v1)
                ))");
  EXPECT_EQ(2, stats.invokes);

  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (invoke-virtual (v0) "LBar;.baz:()V")
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (invoke-virtual (v0) "LBar;.baz:()V")
                  (return-void)
                ))");
  EXPECT_EQ(2, stats.invokes);

  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (invoke-direct (v0) "LFoo;.qux:()LFoo;")
                  (move-result-object v1)
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (const v2 0)
                  (throw v2)
                ))");
  EXPECT_EQ(3, stats.invokes);

  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (invoke-direct (v0) "LFoo;.baz:()V")
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (const v1 0)
                  (throw v1)
                ))");
  EXPECT_EQ(4, stats.invokes);

  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (invoke-direct (v0) "LBar;.baz:()V")
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (invoke-direct (v0) "LBar;.baz:()V")
                  (return-void)
                ))");
  EXPECT_EQ(4, stats.invokes);
}

TEST_F(RemoveUninstantiablesTest, CheckCast) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  ASSERT_TRUE(type::is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(type::is_uninstantiable_class(DexType::get_type("LBar;")));

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (check-cast v0 "LFoo;")
                  (move-result-pseudo-object v1)
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (check-cast v0 "Ljava/lang/Void;")
                  (move-result-pseudo-object v1)
                  (const v0 0)
                  (const v1 0)
                  (return-void)
                ))");
  EXPECT_EQ(1, stats.check_casts);

  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (check-cast v0 "LBar;")
                  (move-result-pseudo-object v1)
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (check-cast v0 "LBar;")
                  (move-result-pseudo-object v1)
                  (return-void)
                ))");
  EXPECT_EQ(1, stats.check_casts);

  // Void is itself uninstantiable, so we can infer that following a check-cast,
  // the registers involved hold null.
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (check-cast v0 "Ljava/lang/Void;")
                  (move-result-pseudo-object v1)
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (check-cast v0 "Ljava/lang/Void;")
                  (move-result-pseudo-object v1)
                  (const v0 0)
                  (const v1 0)
                  (return-void)
                ))");
  EXPECT_EQ(2, stats.check_casts);
}

TEST_F(RemoveUninstantiablesTest, GetField) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  DexField::make_field("LFoo;.a:I")->make_concrete(ACC_PUBLIC);
  DexField::make_field("LBar;.a:I")->make_concrete(ACC_PUBLIC);

  ASSERT_TRUE(type::is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(type::is_uninstantiable_class(DexType::get_type("LBar;")));

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (iget v0 "LBar;.a:I")
                  (move-result-pseudo v1)
                  (iget v0 "LFoo;.a:I")
                  (move-result-pseudo v2)
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (iget v0 "LBar;.a:I")
                  (move-result-pseudo v1)
                  (const v3 0)
                  (throw v3)
                ))");
  EXPECT_EQ(1, stats.field_accesses_on_uninstantiable);
}

TEST_F(RemoveUninstantiablesTest, PutField) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  DexField::make_field("LFoo;.a:I")->make_concrete(ACC_PUBLIC);
  DexField::make_field("LBar;.a:I")->make_concrete(ACC_PUBLIC);

  ASSERT_TRUE(type::is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(type::is_uninstantiable_class(DexType::get_type("LBar;")));

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (const v1 0)
                  (iput v0 v1 "LBar;.a:I")
                  (const v2 0)
                  (iput v0 v2 "LFoo;.a:I")
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (const v1 0)
                  (iput v0 v1 "LBar;.a:I")
                  (const v2 0)
                  (const v3 0)
                  (throw v3)
                ))");
  EXPECT_EQ(1, stats.field_accesses_on_uninstantiable);
}

TEST_F(RemoveUninstantiablesTest, GetUninstantiable) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  DexField::make_field("LBar;.mFoo:LFoo;")->make_concrete(ACC_PUBLIC);
  DexField::make_field("LBar;.sFoo:LFoo;")
      ->make_concrete(ACC_PUBLIC | ACC_STATIC);

  DexField::make_field("LBar;.mBar:LBar;")->make_concrete(ACC_PUBLIC);
  DexField::make_field("LBar;.sBar:LBar;")
      ->make_concrete(ACC_PUBLIC | ACC_STATIC);

  ASSERT_TRUE(type::is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(type::is_uninstantiable_class(DexType::get_type("LBar;")));

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (iget-object v0 "LBar;.mFoo:LFoo;")
                  (move-result-pseudo v1)
                  (iget-object v0 "LBar;.mBar:LBar;")
                  (move-result-pseudo v2)
                  (sget-object "LBar.sFoo:LFoo;")
                  (move-result-pseudo v3)
                  (sget-object "LBar.sBar:LBar;")
                  (move-result-pseudo v4)
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (const v1 0)
                  (iget-object v0 "LBar;.mBar:LBar;")
                  (move-result-pseudo v2)
                  (const v3 0)
                  (sget-object "LBar.sBar:LBar;")
                  (move-result-pseudo v4)
                  (return-void)
                ))");
  EXPECT_EQ(2, stats.get_uninstantiables);
}

TEST_F(RemoveUninstantiablesTest, ReplaceAllWithThrow) {
  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_all_with_throw,
                stats,
                /* ACTUAL */ R"((
                  (load-param-object v0)
                  (const v1 0)
                  (if-eqz v1 :l1)
                  (const v2 1)
                  (return-void)
                  (:l1)
                  (const v2 2)
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (load-param-object v0)
                  (const v3 0)
                  (throw v3)
                ))");
  EXPECT_EQ(1, stats.throw_null_methods);
}

TEST_F(RemoveUninstantiablesTest, RunPass) {
  DexStoresVector dss{DexStore{"test_store"}};

  auto* Foo = def_class("LFoo;", Foo_baz, Foo_qux, Foo_fox);
  auto* Bar = def_class("LBar;", Bar_init, Bar_baz, Bar_qux);
  auto* FooBar = def_class("LFooBar;", FooBar_baz);
  dss.back().add_classes({Foo, Bar, FooBar});
  FooBar->set_super_class(Foo->get_type());

  DexField::make_field("LBar;.mFoo:LFoo;")->make_concrete(ACC_PUBLIC);
  DexField::make_field("LFoo;.mBar:LBar;")->make_concrete(ACC_PUBLIC);

  RemoveUninstantiablesPass pass;
  PassManager pm({&pass});

  ConfigFiles c(Json::nullValue);
  pm.run_passes(dss, c);

  EXPECT_ABSTRACT_METHOD("LFoo;.baz:()V");
  EXPECT_ABSTRACT_METHOD("LFoo;.qux:()LFoo;");
  EXPECT_NO_METHOD_DEF("LFooBar;.baz:()V");

  EXPECT_METHOD("LFoo;.fox:()LFoo;",
                R"((
                  (load-param-object v0)
                  (const v1 0)
                  (throw v1)
                ))");

  EXPECT_METHOD("LBar;.baz:()V",
                R"((
                  (load-param-object v0)
                  (return-void)
                ))");

  EXPECT_METHOD("LBar;.qux:()I",
                R"((
                  (load-param-object v0) ; this
                  (const v1 0)
                  (iput-object v1 v0 "LBar;.mFoo:LFoo;")
                  (if-eqz v1 :else)
                  (const v5 0)
                  (throw v5)
                  (:else)
                  (const v5 0)
                  (throw v5)
                ))");

  const auto& pass_infos = pm.get_pass_info();
  auto rm_uninst =
      std::find_if(pass_infos.begin(), pass_infos.end(), [](const auto& pi) {
        return pi.pass->name() == "RemoveUninstantiablesPass";
      });
  ASSERT_NE(rm_uninst, pass_infos.end());

  EXPECT_EQ(1, rm_uninst->metrics.at("instance_ofs"));
  EXPECT_EQ(1, rm_uninst->metrics.at("invokes"));
  EXPECT_EQ(1, rm_uninst->metrics.at("field_accesses_on_uninstantiable"));
  EXPECT_EQ(1, rm_uninst->metrics.at("abstracted_classes"));
  EXPECT_EQ(2, rm_uninst->metrics.at("abstracted_vmethods"));
  EXPECT_EQ(1, rm_uninst->metrics.at("removed_vmethods"));
  EXPECT_EQ(1, rm_uninst->metrics.at("throw_null_methods"));
  EXPECT_EQ(1, rm_uninst->metrics.at("get_uninstantiables"));
}

TEST_F(RemoveUninstantiablesTest, VoidIsUninstantiable) {
  auto uninstantiable_types = compute_uninstantiable_types();
  EXPECT_TRUE(uninstantiable_types.count(type::java_lang_Void()));
}

TEST_F(RemoveUninstantiablesTest, UnimplementedInterfaceIsUninstantiable) {
  auto foo = def_class("LFoo;");
  foo->set_access(foo->get_access() | ACC_INTERFACE | ACC_ABSTRACT);
  auto uninstantiable_types = compute_uninstantiable_types();
  EXPECT_TRUE(uninstantiable_types.count(foo->get_type()));
}

TEST_F(RemoveUninstantiablesTest,
       UnimplementedInterfaceWithRootMethodIsNotUninstantiable) {
  auto foo = def_class("LFoo;");
  foo->set_access(foo->get_access() | ACC_INTERFACE | ACC_ABSTRACT);
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.root:()Z"));
  method->make_concrete(ACC_PUBLIC | ACC_ABSTRACT, /* is_virtual */ true);
  method->rstate.set_root();
  foo->add_method(method);
  auto uninstantiable_types = compute_uninstantiable_types();
  EXPECT_FALSE(uninstantiable_types.count(foo->get_type()));
}

TEST_F(RemoveUninstantiablesTest,
       UnimplementedAnnotationInterfaceIsNotUninstantiable) {
  auto foo = def_class("LFoo;");
  foo->set_access(foo->get_access() | ACC_INTERFACE | ACC_ABSTRACT |
                  ACC_ANNOTATION);
  auto uninstantiable_types = compute_uninstantiable_types();
  EXPECT_FALSE(uninstantiable_types.count(foo->get_type()));
}

TEST_F(RemoveUninstantiablesTest, ImplementedInterfaceIsNotUninstantiable) {
  auto foo = def_class("LFoo;");
  foo->set_access(foo->get_access() | ACC_INTERFACE | ACC_ABSTRACT);
  auto bar = def_class("LBar;", Bar_init, Bar_baz);
  bar->set_interfaces(DexTypeList::make_type_list({foo->get_type()}));
  auto uninstantiable_types = compute_uninstantiable_types();
  EXPECT_FALSE(uninstantiable_types.count(foo->get_type()));
  EXPECT_FALSE(uninstantiable_types.count(bar->get_type()));
}

TEST_F(RemoveUninstantiablesTest, AbstractClassIsUninstantiable) {
  auto foo = def_class("LFoo;");
  foo->set_access(foo->get_access() | ACC_ABSTRACT);
  auto uninstantiable_types = compute_uninstantiable_types();
  EXPECT_TRUE(uninstantiable_types.count(foo->get_type()));
}

TEST_F(RemoveUninstantiablesTest, ExtendedAbstractClassIsNotUninstantiable) {
  auto foo = def_class("LFoo;");
  foo->set_access(foo->get_access() | ACC_ABSTRACT);
  auto bar = def_class("LBar;", Bar_init);
  bar->set_super_class(foo->get_type());
  auto uninstantiable_types = compute_uninstantiable_types();
  EXPECT_FALSE(uninstantiable_types.count(foo->get_type()));
  EXPECT_FALSE(uninstantiable_types.count(bar->get_type()));
}

TEST_F(RemoveUninstantiablesTest, InvokeInterfaceOnUninstantiable) {
  auto foo = def_class("LFoo;");
  foo->set_access(foo->get_access() | ACC_INTERFACE | ACC_ABSTRACT);

  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));
  create_abstract_method(foo, "abs", void_void);

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (invoke-interface (v0) "LFoo;.abs:()V;")
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (const v1 0)
                  (throw v1)
                ))");
  EXPECT_EQ(1, stats.invokes);
}

TEST_F(RemoveUninstantiablesTest, InvokeSuperOnUninstantiable) {
  auto foo = def_class("LFoo;");
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));
  create_abstract_method(foo, "abs", void_void);

  auto bar = def_class("LBar;");
  bar->set_super_class(foo->get_type());

  RemoveUninstantiablesPass::Stats stats;
  EXPECT_CHANGE(replace_uninstantiable_refs,
                stats,
                /* ACTUAL */ R"((
                  (const v0 0)
                  (invoke-super (v0) "LBar;.abs:()V;")
                  (return-void)
                ))",
                /* EXPECTED */ R"((
                  (const v0 0)
                  (const v1 0)
                  (throw v1)
                ))");
  EXPECT_EQ(1, stats.invokes);
}

TEST_F(RemoveUninstantiablesTest, RunPassInstantiableChildrenDefined) {
  DexStoresVector dss{DexStore{"test_store"}};

  auto* Bar = def_class("LBar;", Bar_init, Bar_baz);
  DexMethod::get_method("LBar;.<init>:()V")->as_def()->set_access(ACC_PUBLIC);
  auto* BarBar = def_class("LBarBar;", BarBar_init, BarBar_baz);
  DexMethod::get_method("LBarBar;.<init>:()V")
      ->as_def()
      ->set_access(ACC_PUBLIC);
  dss.back().add_classes({Bar, BarBar});
  BarBar->set_super_class(Bar->get_type());

  RemoveUninstantiablesPass pass;
  PassManager pm({&pass});

  ConfigFiles c(Json::nullValue);
  pm.run_passes(dss, c);

  EXPECT_ABSTRACT_METHOD("LBar;.baz:()V");

  const auto& pass_infos = pm.get_pass_info();
  auto rm_uninst =
      std::find_if(pass_infos.begin(), pass_infos.end(), [](const auto& pi) {
        return pi.pass->name() == "RemoveUninstantiablesPass";
      });
  ASSERT_NE(rm_uninst, pass_infos.end());

  EXPECT_EQ(1, rm_uninst->metrics.at("abstracted_classes"));
  EXPECT_EQ(1, rm_uninst->metrics.at("abstracted_vmethods"));
  EXPECT_EQ(0, rm_uninst->metrics.at("removed_vmethods"));
  EXPECT_EQ(0, rm_uninst->metrics.at("throw_null_methods"));
  EXPECT_EQ(0, rm_uninst->metrics.at("get_uninstantiables"));
}

} // namespace
