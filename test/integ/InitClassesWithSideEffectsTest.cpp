/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InitClassesWithSideEffects.h"
#include "AnnoUtils.h"
#include "MethodUtil.h"
#include "RedexTest.h"
#include "Show.h"

class InitClassesWithSideEffectsTest : public RedexIntegrationTest {
 public:
  InitClassesWithSideEffectsTest() {
    // We need to define a few external methods...
    auto m1 = DexMethod::make_method(
                  "Ljava/lang/System;.loadLibrary:(Ljava/lang/String;)V")
                  ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    m1->set_deobfuscated_name(show(m1));
    auto m2 = DexMethod::make_method("Ljava/lang/Math;.max:(II)I")
                  ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    m2->set_deobfuscated_name(show(m2));
    auto m3 = DexMethod::make_method("Ljava/lang/Object;.<init>:()V")
                  ->make_concrete(ACC_PUBLIC | ACC_CONSTRUCTOR, false);
    m3->set_deobfuscated_name(show(m3));
  }
};

TEST_F(InitClassesWithSideEffectsTest, test) {
  // Check if attributes on all classes represent inferred side-effects.
  auto scope = build_class_scope(stores);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ true);
  auto annotation = DexType::get_type("Lcom/facebook/redextest/HasSideffects;");
  for (auto cls : scope) {
    auto has_side_effect =
        !!init_classes_with_side_effects.refine(cls->get_type());
    EXPECT_EQ(has_side_effect, get_annotation(cls, annotation) != nullptr)
        << show(cls) << " has_side_effect = " << has_side_effect;
  }
}
