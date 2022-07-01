/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AppModuleUsage.h"
#include "RedexTest.h"

namespace {
// @UsesAppModule DexType descriptor
constexpr const char* USES_AM_ANNO_DESCRIPTOR = "LUsesAppModule;";
DexType* annotation() { return DexType::get_type(USES_AM_ANNO_DESCRIPTOR); }
} // namespace

struct UsesAppModuleTest : public RedexIntegrationTest {};

TEST_F(UsesAppModuleTest, testNoneMethod) {
  auto method1 = DexMethod::get_method("LUsesAppModuleAnnotated;.method0:()V");
  auto method1_modules =
      AppModuleUsagePass::get_modules_used(method1->as_def(), annotation());
  ASSERT_EQ(method1_modules.size(), 0);
}

TEST_F(UsesAppModuleTest, testSingleMethod) {
  auto method1 = DexMethod::get_method("LUsesAppModuleAnnotated;.method1:()V");
  auto method1_modules =
      AppModuleUsagePass::get_modules_used(method1->as_def(), annotation());
  ASSERT_EQ(method1_modules.size(), 1);
  ASSERT_GT(method1_modules.count("AppModule"), 0);
}

TEST_F(UsesAppModuleTest, testListMethod) {
  auto method2 = DexMethod::get_method("LUsesAppModuleAnnotated;.method2:()V");
  auto method2_modules =
      AppModuleUsagePass::get_modules_used(method2->as_def(), annotation());
  ASSERT_EQ(method2_modules.size(), 2);
  ASSERT_GT(method2_modules.count("AppModule"), 0);
  ASSERT_GT(method2_modules.count("classes"), 0);
}

TEST_F(UsesAppModuleTest, testSingleField) {
  auto field = DexField::get_field(
      "LUsesAppModuleAnnotated;.field:LAppModuleUsageOtherClass;");
  auto field_modules =
      AppModuleUsagePass::get_modules_used(field->as_def(), annotation());
  ASSERT_EQ(field_modules.size(), 1);
  ASSERT_GT(field_modules.count("AppModule"), 0);
}

TEST_F(UsesAppModuleTest, testListField) {
  auto field = DexField::get_field(
      "LUsesAppModuleAnnotated;.field2:LAppModuleUsageOtherClass;");
  auto field_modules =
      AppModuleUsagePass::get_modules_used(field->as_def(), annotation());
  ASSERT_EQ(field_modules.size(), 2);
  ASSERT_GT(field_modules.count("AppModule"), 0);
  ASSERT_GT(field_modules.count("classes"), 0);
}

TEST_F(UsesAppModuleTest, testSingleType) {
  auto* volt_use_class =
      type_class(DexType::get_type("LUsesAppModuleAnnotated;"));
  auto volt_use_class_modules =
      AppModuleUsagePass::get_modules_used(volt_use_class, annotation());
  ASSERT_EQ(volt_use_class_modules.size(), 1);
  ASSERT_GT(volt_use_class_modules.count("AppModule"), 0);
}

TEST_F(UsesAppModuleTest, testListType) {
  auto* volt_use_other_class =
      type_class(DexType::get_type("LAppModuleUsageOtherClass;"));
  auto volt_use_other_class_modules =
      AppModuleUsagePass::get_modules_used(volt_use_other_class, annotation());
  ASSERT_EQ(volt_use_other_class_modules.size(), 2);
  ASSERT_GT(volt_use_other_class_modules.count("AppModule"), 0);
  ASSERT_GT(volt_use_other_class_modules.count("classes"), 0);
}
