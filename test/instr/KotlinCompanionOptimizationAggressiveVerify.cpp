/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "verify/VerifyUtil.h"

namespace {
constexpr const char* class_foo = "LFoo;";
} // namespace

// Test cls LCompanionClass; PreVerify has been tested in
// KotlinCompanionOptimizationVerify.cpp.
TEST_F(PostVerify, CompanionClass) {
  auto* outer_cls = find_class_named(classes, "LCompanionClass;");
  auto* companion_cls = find_class_named(classes, "LCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  // After opt, there is no new-instance for LCompanionClass clinit
  ASSERT_EQ(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // After opt, "hello", "hello1", "getS" has been inlined.
  auto* meth_main = find_vmethod_named(*foo_cls, "main");
  EXPECT_NE(nullptr, meth_main);
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "hello"));
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "hello"));
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "hello1"));
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "hello1"));
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "getS"));
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "getS"));
  // After opt, there is no sfield "Companion" in outer class.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
}

// Test cls LAnotherCompanionClass;
TEST_F(PostVerify, AnotherCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LAnotherCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LAnotherCompanionClass$Test;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // After opt, there is no new-instance for LAnotherCompanionClass clinit
  ASSERT_EQ(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // After opt, in main fun, there should be one static invoke for funX.
  auto* meth_main = find_vmethod_named(*foo_cls, "main");
  EXPECT_NE(nullptr, meth_main);
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "funX"));
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "funX"));
  // After opt, there is no sfield "Test" in outer class.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Test"));
  // After opt, method "funX" is relocated and removed.
  EXPECT_EQ(nullptr, find_dmethod_named(*outer_cls, "funX"));
}

// Test cls LThirdCompanionClass;
TEST_F(PostVerify, ThirdCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LThirdCompanionClass;");
  auto* companion_cls = find_class_named(classes, "LThirdCompanionClass$Test;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  // After opt, there is no new-instance in LThirdCompanionClass clinit, and in
  // fact the now trivial clinit is removed
  ASSERT_EQ(nullptr, meth_clinit);
  // After opt, in LThirdCompanionClass; sfield "Test" should be removed.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Test"));
  // After opt, method "access$funY" and "funY" should be relocated from
  // companion class to outer class and then removed.
  EXPECT_EQ(nullptr, find_dmethod_named(*outer_cls, "access$funY"));
  EXPECT_EQ(nullptr, find_dmethod_named(*outer_cls, "funY"));
}

// Companion with outer-class sfields — optimized by the pass (the backing
// field for `counter` lives on the outer class, not on the companion). With the
// aggressive pipeline, the companion class is further removed by other passes.
TEST_F(PostVerify, CompanionWithSfields) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithSfields;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithSfields$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
}

// Companion with outer-class <clinit> — optimized by the pass (the <clinit>
// and backing field for `computed` live on the outer class, not on the
// companion). With the aggressive pipeline, the companion class is further
// removed by other passes.
TEST_F(PostVerify, CompanionWithClinit) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithClinit;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithClinit$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
}

// Named object declaration — not touched by KotlinCompanionOptimizationPass.
// With the aggressive pipeline, the class survives (holds live static fields)
// but INSTANCE may be removed after method inlining.
TEST_F(PostVerify, NamedObjectDeclaration) {
  auto* cls = find_class_named(classes, "LNamedObjectDeclaration;");
  EXPECT_NE(nullptr, cls);
}

// Nested named object declaration — not touched by
// KotlinCompanionOptimizationPass. With the aggressive pipeline, the class
// survives (holds live static fields).
TEST_F(PostVerify, NestedObjectDeclaration) {
  auto* outer_cls = find_class_named(classes, "LOuterWithObject;");
  auto* nested_cls = find_class_named(classes, "LOuterWithObject$NestedObj;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, nested_cls);
}
