/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Resolver.h"
#include "Show.h"
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
  auto field = find_sfield_named(*outer_cls, "Companion");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
}

TEST_F(PostVerify, CompanionClass) {
  auto* outer_cls = find_class_named(classes, "LCompanionClass;");
  auto* companion_cls = find_class_named(classes, "LCompanionClass$Companion;");
  auto* foo_cls = find_class_named(classes, class_foo);
  // KotlinObjectInliner Pass only relocate method from companion object to its
  // outer class. Therefore, the companion cls is still there.
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
  // After opt, method "hello", "hello1" and "getS" are relocated from campanion
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
      find_class_named(classes, "LAnotherCompanionClass$Test;");
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
  // Before opt, in LCompanionClass; there should be a sfield "Test" with type
  // 'LAnotherCompanionClass$Test;'
  auto field = find_sfield_named(*outer_cls, "Test");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
}

TEST_F(PostVerify, AnotherCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LAnotherCompanionClass;");
  auto* companion_cls =
      find_class_named(classes, "LAnotherCompanionClass$Test;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);
  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // After opt, there is no new-instance for LCompanionClass clinit
  ASSERT_EQ(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // After opt, in main fun, there should be one static invoke for funX.
  auto* meth_main = find_vmethod_named(*foo_cls, "main");
  EXPECT_NE(nullptr, meth_main);
  EXPECT_EQ(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_VIRTUAL, "funX"));
  EXPECT_NE(nullptr, find_invoke(meth_main, DOPCODE_INVOKE_STATIC, "funX"));
  // After opt, there is no sfield "Test" in outer class.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Test"));
  // After opt, method "funX" is relocated from campanion class to outer class.
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "funX"));
  EXPECT_EQ(nullptr, find_vmethod_named(*companion_cls, "funX"));
}

// Test cls LThirdCompanionClass;
TEST_F(PreVerify, ThirdCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LThirdCompanionClass;");
  auto* companion_cls = find_class_named(classes, "LThirdCompanionClass$Test;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);
  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // Before opt, there is a new-instance for LThirdCompanionClass$Companion;
  ASSERT_NE(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // Before opt, in LThirdCompanionClass; there should be a sfield "Test" with
  // type 'LThirdCompanionClass$Companion;'
  auto field = find_sfield_named(*outer_cls, "Test");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
  // In 'LThirdCompanionClass$Companion;', since funY is marked as priviate,
  // another method , a dmethod 'access$funY' is generated for outer class
  // accesing funY.
  auto* meth_access_funY = find_dmethod_named(*companion_cls, "access$funY");
  auto* meth_funY = find_dmethod_named(*companion_cls, "funY");
  EXPECT_NE(nullptr, meth_access_funY);
  EXPECT_NE(nullptr, meth_funY);
  EXPECT_NE(nullptr,
            find_invoke(meth_access_funY, DOPCODE_INVOKE_DIRECT, "funY"));
}

TEST_F(PostVerify, ThirdCompanionClass) {
  auto* outer_cls = find_class_named(classes, "LThirdCompanionClass;");
  auto* companion_cls = find_class_named(classes, "LThirdCompanionClass$Test;");
  auto* foo_cls = find_class_named(classes, class_foo);
  EXPECT_NE(nullptr, outer_cls);
  EXPECT_NE(nullptr, companion_cls);
  EXPECT_NE(nullptr, foo_cls);

  auto* meth_clinit = find_dmethod_named(*outer_cls, "<clinit>");
  ASSERT_NE(nullptr, meth_clinit);
  // After opt, there is no new-instance in LThirdCompanionClass clinit
  ASSERT_EQ(nullptr, find_instruction(meth_clinit, DOPCODE_NEW_INSTANCE));
  // After opt, in LThirdCompanionClass; sfield "Test" should be removed.
  EXPECT_EQ(nullptr, find_sfield_named(*outer_cls, "Test"));
  // After opt, method "access$funY" and "funY" should be relocated from
  // campanion class to outer class.
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "access$funY"));
  EXPECT_EQ(nullptr, find_dmethod_named(*companion_cls, "access$funY"));
  EXPECT_NE(nullptr, find_dmethod_named(*outer_cls, "funY"));
  EXPECT_EQ(nullptr, find_dmethod_named(*companion_cls, "funY"));
}

// Test AnnoClass. This type of Companion class contains static field, so won't
// be handled by current KotlinObjectInliner pass. However, this type of
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
  auto field = find_sfield_named(*outer_cls, "Companion");
  EXPECT_NE(nullptr, field);
  EXPECT_EQ(field->get_type(), companion_cls->get_type());
}
