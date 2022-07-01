/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnnoUtils.h"
#include "MethodUtil.h"
#include "RedexTest.h"
#include "Show.h"

class ClinitSideEffectTest : public RedexIntegrationTest {};

TEST_F(ClinitSideEffectTest, test) {
  auto scope = build_class_scope(stores);
  auto annotation = DexType::get_type("Lcom/facebook/redextest/HasSideffects;");
  for (auto cls : scope) {
    auto has_side_effect = !!method::clinit_may_have_side_effects(
        cls, /* allow_benign_method_invocations */ false);
    EXPECT_EQ(has_side_effect, get_annotation(cls, annotation) != nullptr)
        << show(cls) << " has_side_effect = " << has_side_effect;
  }
}
