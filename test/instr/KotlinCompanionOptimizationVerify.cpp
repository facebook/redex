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

// Test cls LCompanionClass;
TEST_F(PreVerify, CompanionClass) {
  auto* outer_cls = find_class_named(classes, "LCompanionClass;");
  auto* companion_cls = find_class_named(classes, "LCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // Before opt, there is a new-instance for LCompanionClass$Companion;
  ASSERT_NE(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // Before opt, in main fun, first load an instance of the
  // LCompanionClass$Companion; class from a static Companion field on
  // LCompanionClass; Then makes a virtual method call to the hello function on
  // that instance. Same as hello1 and getS.
  auto* meth_main = find_vmethod_named(*foo_cls, "main");
  EXPECT_NE(nullptr, meth_main);
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "hello"));
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "hello1"));
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "getS"));
  // Before opt, in LCompanionClass; there should be a sfield "Companion" with
  // type 'LCompanionClass$Companion;'
  auto* field = find_sfield_named(*outer_cls, "Companion");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
}

TEST_F(PostVerify, CompanionClass) {
  auto* outer_cls = find_class_named(classes, "LCompanionClass;");
  auto* companion_cls = find_class_named(classes, "LCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  // KotlinCompanionOptimizationPass only relocates methods from companion
  // object to its outer class. Therefore, the companion cls is still there.
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  // After opt, there is no new-instance for LCompanionClass clinit
  ASSERT_EQ(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // After opt, there should be a static call "hello", "hello1", "getS".
  auto* meth_main = find_vmethod_named(*foo_cls, "main");
  EXPECT_NE(nullptr, meth_main);
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "hello"));
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "hello"));
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "hello1"));
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "hello1"));
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "getS"));
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "getS"));
  // After opt, there is no sfield "Companion" in outer class.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // After opt, method "hello", "hello1" and "getS" are relocated from companion
  // class to outer class.
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "hello"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "hello"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "hello1"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "hello1"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "getS"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "getS"));
}

// Test cls LAnotherCompanionClass;
TEST_F(PreVerify, AnotherCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LAnotherCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LAnotherCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // Before opt, there is a new-instance for LAnotherCompanionClass$Companion;
  ASSERT_NE(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // Before opt, in main fun, there is one virtual invoke for funX, one for
  // hello1.
  auto* meth_main = find_vmethod_named(*foo_cls, "main");
  EXPECT_NE(nullptr, meth_main);
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "funX"));
  // Before opt, in LAnotherCompanionClass; there should be a sfield "Companion"
  // with type 'LAnotherCompanionClass$Companion;'
  auto* field = find_sfield_named(*outer_cls, "Companion");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
}

TEST_F(PostVerify, AnotherCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LAnotherCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LAnotherCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);
  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // After opt, there is no new-instance for LAnotherCompanionClass clinit
  ASSERT_EQ(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // After opt, in main fun, there should be one static invoke for funX.
  auto* meth_main = find_vmethod_named(*foo_cls, "main");
  EXPECT_NE(nullptr, meth_main);
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "funX"));
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "funX"));
  // After opt, there is no sfield "Companion" in outer class.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // After opt, method "funX" is relocated from companion class to outer class.
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "funX"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "funX"));
}

// Test cls LThirdCompanionClass;
TEST_F(PreVerify, ThirdCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LThirdCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LThirdCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);
  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // Before opt, there is a new-instance for LThirdCompanionClass$Companion;
  ASSERT_NE(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // Before opt, in LThirdCompanionClass; there should be a sfield "Companion"
  // with type 'LThirdCompanionClass$Companion;'
  auto* field = find_sfield_named(*outer_cls, "Companion");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
  // In 'LThirdCompanionClass$Companion;', since funY is marked as private,
  // another method, a dmethod 'access$funY' is generated for outer class
  // accessing funY.
  auto* meth_access_funY = find_dmethod_named(*companion_cls, "access$funY");
  auto* meth_funY = find_dmethod_named(*companion_cls, "funY");
  EXPECT_NE(nullptr, meth_access_funY);
  EXPECT_NE(nullptr, meth_funY);
  EXPECT_NE(nullptr,
            find_invoke(meth_access_funY, DOPCODE_INVOKE_DIRECT, "funY"));
}

TEST_F(PostVerify, ThirdCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LThirdCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LThirdCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // After opt, there is no new-instance in LThirdCompanionClass clinit
  ASSERT_EQ(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // After opt, in LThirdCompanionClass; sfield "Companion" should be removed.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // After opt, method "access$funY" and "funY" should be relocated from
  // companion class to outer class.
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "access$funY"));
  EXPECT_EQ(nullptr, find_dmethod_named(*companion_cls, "access$funY"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "funY"));
  EXPECT_EQ(nullptr, find_dmethod_named(*companion_cls, "funY"));
}

// Test AnnoClass. This type of Companion class contains static field, so won't
// be handled by current KotlinCompanionOptimizationPass. However, this type of
// companion object can be opted by AnnoKill+RUM pass. Once static fields are
// supported, this test should be updated.
TEST_F(PostVerify, AnnoClass) {
  auto* outer_cls = find_class_named(classes, "LAnnoClass;");
  auto* companion_cls = find_class_named(classes, "LAnnoClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  // After opt, there is still Companion obj in LAnnoClass static fields.
  auto* field = find_sfield_named(*outer_cls, "Companion");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
}

// Companion with outer-class sfields — optimized by the pass. The backing
// field for `counter` lives on the outer class, not on the companion, so
// the companion has no sfields and is eligible.
TEST_F(PostVerify, CompanionWithSfields) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithSfields;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithSfields$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  // After opt, the Companion sfield is removed.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // After opt, increment is relocated from companion to outer class.
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "increment"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "increment"));
}

// Companion with outer-class <clinit> — optimized by the pass. The <clinit>
// and backing field for `computed` live on the outer class, not on the
// companion, so the companion has no clinit and is eligible.
TEST_F(PostVerify, CompanionWithClinit) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithClinit;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithClinit$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  // After opt, the Companion sfield is removed.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
}

// Named object declaration — must not be touched by the pass
TEST_F(PostVerify, NamedObjectDeclaration) {
  auto* cls = find_class_named(classes, "LNamedObjectDeclaration;");
  EXPECT_NE(nullptr, cls);
  auto* field = find_sfield_named(*cls, "INSTANCE");
  EXPECT_NE(nullptr, field);
}

// Nested named object declaration — must not be touched by the pass
TEST_F(PostVerify, NestedObjectDeclaration) {
  auto* outer_cls = find_class_named(classes, "LOuterWithObject;");
  auto* nested_cls = find_class_named(classes, "LOuterWithObject$NestedObj;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, nested_cls);
  auto* field = find_sfield_named(*nested_cls, "INSTANCE");
  EXPECT_NE(nullptr, field);
}

// Companion with const val — the companion has a <clinit> for $$INSTANCE.
// The sget/sput pattern in the clinit must be allowlisted by is_def_trackable.
TEST_F(PostVerify, CompanionWithConstVal) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithConstVal;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithConstVal$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  // After opt, the Companion sfield is removed.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // After opt, getMagic is relocated from companion to outer class.
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "getMagic"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "getMagic"));
}

// Method name collision: the companion's get() is relocated to the outer class.
// Since the outer class already has a virtual get(), the relocated companion
// get() is renamed to avoid collision.
TEST_F(PostVerify, CompanionWithMethodCollision) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithMethodCollision;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithMethodCollision$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // The companion's get() should no longer be on the companion class.
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "get"));
}

// Companion inter-calls: methodA and methodB call each other.
// Both should be relocated as static methods.
TEST_F(PostVerify, CompanionWithInterCalls) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithInterCalls;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithInterCalls$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "methodA"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "methodA"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "methodB"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "methodB"));
}

// @JvmStatic bridge: the companion's compute() is relocated, and the
// existing @JvmStatic bridge on the outer class is renamed.
TEST_F(PostVerify, CompanionWithJvmStaticBridge) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithJvmStaticBridge;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithJvmStaticBridge$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // The companion's compute() should now be on the outer class.
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "compute"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "compute"));
  // The old @JvmStatic bridge should be renamed.
  EXPECT_NE(nullptr,
            find_dmethod_named(*outer_cls, "compute$companion_bridge"));
}

// Companion with default arguments: both greet and greet$default are relocated.
TEST_F(PostVerify, CompanionWithDefaults) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithDefaults;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithDefaults$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "greet"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "greet"));
}

// Abstract outer class: companion methods are still relocated as static
// methods.
TEST_F(PostVerify, AbstractOuterClass) {
  auto* outer_cls = find_class_named(classes, "LAbstractOuterClass;");
  auto* companion_cls =
      find_class_named(classes, "LAbstractOuterClass$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "helperFunc"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "helperFunc"));
}

// Pure function companion: double(int) is relocated.
TEST_F(PostVerify, CompanionWithPureFunction) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithPureFunction;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithPureFunction$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Companion"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "double"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "double"));
}

// Companion whose instance escapes: doWork should NOT be relocated because
// getCompanion() returns the companion instance.
TEST_F(PostVerify, CompanionEscapes) {
  auto* companion_cls =
      find_class_named(classes, "LCompanionEscapes$Companion;");
  EXPECT_NE(nullptr, companion_cls);
  // doWork should still be on the companion class.
  EXPECT_NE(nullptr, find_vmethod_named(*companion_cls, "doWork"));
}

// Companion with a kept method: must NOT be relocated because the companion
// has a method with a ProGuard -keep rule (simulating @DoNotStrip / JNI).
TEST_F(PostVerify, CompanionWithKeptMethodNotRelocated) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithKeptMethod;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithKeptMethod$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  // Companion sfield should still be present — companion not relocated.
  EXPECT_NE(nullptr, find_sfield_named(*outer_cls, "Companion"));
  // keptMethod should still be on the companion class.
  EXPECT_NE(nullptr, find_vmethod_named(*companion_cls, "keptMethod"));
}

// @Synchronized companion: must NOT be relocated because @Synchronized
// generates MONITOR_ENTER on the companion instance.
TEST_F(PostVerify, CompanionWithSynchronizedNotRelocated) {
  auto* outer_cls = find_class_named(classes, "LCompanionWithSynchronized;");
  auto* companion_cls =
      find_class_named(classes, "LCompanionWithSynchronized$Companion;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  // The Companion sfield should still be present — companion not relocated.
  EXPECT_NE(nullptr, find_sfield_named(*outer_cls, "Companion"));
}

// Named companion object — must not be relocated by the pass because the inner
// class name (NamedCompanionClass$Custom) does not end with $Companion.
TEST_F(PostVerify, NamedCompanionNotRelocated) {
  auto* outer_cls = find_class_named(classes, "LNamedCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LNamedCompanionClass$Custom;");
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  // The "Custom" sfield should still be present on the outer class.
  auto* field = find_sfield_named(*outer_cls, "Custom");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
  // funZ should still be on the companion class, not relocated to outer.
  EXPECT_NE(nullptr, find_vmethod_named(*companion_cls, "funZ"));
  EXPECT_EQ(nullptr, find_dmethod_named(*outer_cls, "funZ"));
}
