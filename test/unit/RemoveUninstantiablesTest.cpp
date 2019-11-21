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

namespace {

class RemoveUninstantiablesTest : public RedexTest {};

/// Expect \c RemoveUninstantiablesPass to convert \p ACTUAL into \p EXPECTED
/// where both parameters are strings containing IRCode in s-expression form.
#define EXPECT_CHANGE(OPERATION, ACTUAL, EXPECTED)                    \
  do {                                                                \
    auto actual_ir = assembler::ircode_from_string(ACTUAL);           \
    const auto expected_ir = assembler::ircode_from_string(EXPECTED); \
                                                                      \
    actual_ir->build_cfg();                                           \
    RemoveUninstantiablesPass::OPERATION(actual_ir->cfg());           \
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

/**
 * int qux(Foo foo) {
 *   int ret;
 *   if (foo.qux() instanceof Foo) {
 *     ret = 42;
 *   } else {
 *     ret = 43;
 *   }
 *   return ret;
 * }
 */
const char* const Bar_qux = R"(
(method (public) "LBar;.qux:(LFoo;)I"
  ((load-param-object v0) ; this
   (load-param-object v1) ; foo
   (invoke-virtual (v1) "LFoo;.qux:()LFoo;")
   (move-result-object v2)
   (instance-of v2 "LFoo;")
   (move-result-pseudo v3)
   (if-eqz v3 :else)
   (const v4 42)
   (goto :end)
   (:else)
   (const v4 43)
   (:end)
   (return v4))
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

TEST_F(RemoveUninstantiablesTest, InstanceOf) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  ASSERT_TRUE(is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(is_uninstantiable_class(DexType::get_type("LBar;")));

  EXPECT_CHANGE(replace_uninstantiable_refs,
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
}

TEST_F(RemoveUninstantiablesTest, Invoke) {
  def_class("LFoo;", Foo_baz, Foo_qux);
  def_class("LBar;", Bar_init, Bar_baz);

  ASSERT_TRUE(is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(is_uninstantiable_class(DexType::get_type("LBar;")));

  EXPECT_CHANGE(replace_uninstantiable_refs,
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

  EXPECT_CHANGE(replace_uninstantiable_refs,
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

  EXPECT_CHANGE(replace_uninstantiable_refs,
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

  EXPECT_CHANGE(replace_uninstantiable_refs,
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

  EXPECT_CHANGE(replace_uninstantiable_refs,
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

  EXPECT_CHANGE(replace_uninstantiable_refs,
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
}

TEST_F(RemoveUninstantiablesTest, GetField) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  DexField::make_field("LFoo;.a:I")->make_concrete(ACC_PUBLIC);
  DexField::make_field("LBar;.a:I")->make_concrete(ACC_PUBLIC);

  ASSERT_TRUE(is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(is_uninstantiable_class(DexType::get_type("LBar;")));

  EXPECT_CHANGE(replace_uninstantiable_refs,
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
}

TEST_F(RemoveUninstantiablesTest, PutField) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  DexField::make_field("LFoo;.a:I")->make_concrete(ACC_PUBLIC);
  DexField::make_field("LBar;.a:I")->make_concrete(ACC_PUBLIC);

  ASSERT_TRUE(is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(is_uninstantiable_class(DexType::get_type("LBar;")));

  EXPECT_CHANGE(replace_uninstantiable_refs,
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

  ASSERT_TRUE(is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(is_uninstantiable_class(DexType::get_type("LBar;")));

  EXPECT_CHANGE(replace_uninstantiable_refs,
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
}

TEST_F(RemoveUninstantiablesTest, ReplaceAllWithThrow) {
  EXPECT_CHANGE(replace_all_with_throw,
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
}

TEST_F(RemoveUninstantiablesTest, RunPass) {
  DexStoresVector dss{{"test_store"}};

  auto* Foo = def_class("LFoo;", Foo_baz, Foo_qux);
  auto* Bar = def_class("LBar;", Bar_init, Bar_baz, Bar_qux);
  dss.back().add_classes({Foo, Bar});

  RemoveUninstantiablesPass pass;
  PassManager pm({&pass});

  ConfigFiles c(Json::nullValue);
  pm.run_passes(dss, c);

  EXPECT_METHOD("LFoo;.baz:()V",
                R"((
                  (load-param-object v0)
                  (const v1 0)
                  (throw v1)
                ))");

  EXPECT_METHOD("LFoo;.qux:()LFoo;",
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

  EXPECT_METHOD("LBar;.qux:(LFoo;)I",
                R"((
                  (load-param-object v0)
                  (load-param-object v1)
                  (const v5 0)
                  (throw v5)
                ))");
}

} // namespace
