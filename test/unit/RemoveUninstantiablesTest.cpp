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
#define EXPECT_CHANGE(ACTUAL, EXPECTED)                               \
  do {                                                                \
    auto actual_ir = assembler::ircode_from_string(ACTUAL);           \
    const auto expected_ir = assembler::ircode_from_string(EXPECTED); \
                                                                      \
    actual_ir->build_cfg();                                           \
    RemoveUninstantiablesPass::remove_from_cfg(actual_ir->cfg());     \
    actual_ir->clear_cfg();                                           \
                                                                      \
    EXPECT_CODE_EQ(expected_ir.get(), actual_ir.get());               \
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
(method (public static) "LBar;.<init>:()V"
  ((return-void))
))";

const char* const Bar_baz = R"(
(method (public) "LBar;.baz:()V"
  ((return-void))
))";

const char* const Foo_baz = R"(
(method (public) "LFoo;.baz:()V"
  ((return-void))
))";

const char* const Foo_qux = R"(
(method (public) "LFoo;.qux:()LFoo;"
  ((return-object v0))
))";

TEST_F(RemoveUninstantiablesTest, InstanceOf) {
  def_class("LFoo;");
  def_class("LBar;", Bar_init);

  ASSERT_TRUE(is_uninstantiable_class(DexType::get_type("LFoo;")));
  ASSERT_FALSE(is_uninstantiable_class(DexType::get_type("LBar;")));

  EXPECT_CHANGE(
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

  EXPECT_CHANGE(
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

  EXPECT_CHANGE(
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

  EXPECT_CHANGE(
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

  EXPECT_CHANGE(
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

  EXPECT_CHANGE(
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

  EXPECT_CHANGE(
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

} // namespace
