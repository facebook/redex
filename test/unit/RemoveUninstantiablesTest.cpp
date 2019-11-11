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

TEST_F(RemoveUninstantiablesTest, InstanceOf) {
  assembler::class_with_methods("LFoo;", {});
  assembler::class_with_method("LBar;", R"(
    (method (public static) "LBar;.<init>:()V"
      ((return-void))
    )
  )");

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

} // namespace
