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
      find_class_named(classes, "LAnotherCompanionClass$Companion;");
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
  // After opt, there is no sfield "Companion" in outer class.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // After opt, method "funX" is relocated and removed.
  EXPECT_EQ(nullptr, find_dmethod_named(*outer_cls, "funX"));
}

// Test cls LThirdCompanionClass;
TEST_F(PostVerify, ThirdCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LThirdCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LThirdCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  // After opt, there is no new-instance in LThirdCompanionClass clinit, and in
  // fact the now trivial clinit is removed
  ASSERT_EQ(nullptr, meth_clinit);
  // After opt, in LThirdCompanionClass; sfield "Companion" should be removed.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
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

// Method name collision: after aggressive pipeline, companion class is removed
// and methods are inlined.
TEST_F(PostVerify, CompanionWithMethodCollision) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithMethodCollision;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithMethodCollision$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
}

// Companion inter-calls: after aggressive pipeline, companion class is removed.
TEST_F(PostVerify, CompanionWithInterCalls) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithInterCalls;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithInterCalls$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
}

// @JvmStatic bridge: after aggressive pipeline, companion class is removed.
// The outer class may also be removed if all its methods are inlined.
TEST_F(PostVerify, CompanionWithJvmStaticBridge) {
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithJvmStaticBridge$Companion;");
  EXPECT_EQ(nullptr, companion_cls);
  auto* outer_cls = find_class_named(classes, "LCompanionWithJvmStaticBridge;");
  if (outer_cls != nullptr) {
    EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  }
}

// Default args: after aggressive pipeline, companion class is removed.
TEST_F(PostVerify, CompanionWithDefaults) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithDefaults;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithDefaults$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
}

// Abstract outer class: after aggressive pipeline, companion class is removed.
TEST_F(PostVerify, AbstractOuterClass) {
  auto* outer_cls = find_class_named(classes, "LAbstractOuterClass;");
  auto* companion_cls =
      find_class_named(classes, "LAbstractOuterClass$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_EQ(nullptr, companion_cls);
}

// Pure function: after aggressive pipeline, both classes may be removed
// entirely if all usages are inlined.
TEST_F(PostVerify, CompanionWithPureFunction) {
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithPureFunction$Companion;");
  EXPECT_EQ(nullptr, companion_cls);
}

// Const val: after aggressive pipeline, both classes may be removed
// entirely if all usages are inlined.
TEST_F(PostVerify, CompanionWithConstVal) {
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithConstVal$Companion;");
  EXPECT_EQ(nullptr, companion_cls);
}

// Named companion object — not relocated by KotlinCompanionOptimizationPass
// because the inner class name (NamedCompanionClass$Custom) does not end with
// $Companion. The companion class should survive the aggressive pipeline.
TEST_F(PostVerify, NamedCompanionNotRelocated) {
  auto* outer_cls = find_class_named(classes, "LNamedCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LNamedCompanionClass$Custom;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  // funZ should not have been relocated to the outer class.
  EXPECT_EQ(nullptr, find_dmethod_named(*outer_cls, "funZ"));
}
