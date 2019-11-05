/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApiLevelsUtils.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "ScopeHelper.h"

namespace {

std::vector<DexClass*> create_scope(bool add_parent) {
  std::vector<DexClass*> scope = create_empty_scope();
  auto obj_t = type::java_lang_Object();

  auto a_t = DexType::make_type("Landroidx/ArrayMap;");
  auto a_cls = create_internal_class(a_t, obj_t, {});
  scope.push_back(a_cls);

  auto b_t = DexType::make_type("Landroidx/ArraySet;");
  auto b_cls = create_internal_class(b_t, obj_t, {});
  scope.push_back(b_cls);

  auto c_t = DexType::make_type("Landroidx/LongSparseArray;");
  auto c_cls = create_internal_class(c_t, a_t, {});
  scope.push_back(c_cls);

  if (add_parent) {
    auto d_t = DexType::make_type("Landroidx/ArraySetParentClass;");
    auto d_cls = create_internal_class(d_t, obj_t, {});
    b_cls->set_super_class(d_t);
    scope.push_back(d_cls);
  }

  return scope;
}

} // namespace

TEST(ApiUtilsTest, testParseInputFormat) {
  g_redex = new RedexContext();

  Scope scope = create_scope(false);
  api::ApiLevelsUtils api_utils(scope, std::getenv("api_utils_easy_input_path"),
                                21);
  const auto& framework_cls_to_api = api_utils.get_framework_classes();
  EXPECT_EQ(framework_cls_to_api.size(), 4);

  auto a_t = DexType::make_type("Landroid/util/ArrayMap;");
  EXPECT_EQ(framework_cls_to_api.at(a_t).mrefs.size(), 2);
  EXPECT_EQ(framework_cls_to_api.at(a_t).frefs.size(), 0);

  auto b_t = DexType::make_type("Landroid/util/ArraySet;");
  EXPECT_EQ(framework_cls_to_api.at(b_t).mrefs.size(), 2);

  auto c_t = DexType::make_type("Landroid/util/LongSparseArray;");
  EXPECT_EQ(framework_cls_to_api.at(c_t).mrefs.size(), 0);

  delete g_redex;
}

TEST(ApiUtilsTest, testEasyInput_EasyReleaseLibraries) {
  g_redex = new RedexContext();

  Scope scope = create_scope(false);
  api::ApiLevelsUtils api_utils(scope, std::getenv("api_utils_easy_input_path"),
                                21);

  const auto& types_to_framework_api = api_utils.get_types_to_framework_api();
  EXPECT_EQ(types_to_framework_api.size(), 3);

  auto a_framework = DexType::make_type("Landroid/util/ArrayMap;");
  auto a_release = DexType::make_type("Landroidx/ArrayMap;");
  EXPECT_EQ(types_to_framework_api.at(a_release).cls, a_framework);

  auto b_framework = DexType::make_type("Landroid/util/ArraySet;");
  auto b_release = DexType::make_type("Landroidx/ArraySet;");
  EXPECT_EQ(types_to_framework_api.at(b_release).cls, b_framework);

  auto c_framework = DexType::make_type("Landroid/util/LongSparseArray;");
  auto c_release = DexType::make_type("Landroidx/LongSparseArray;");
  EXPECT_EQ(types_to_framework_api.at(c_release).cls, c_framework);
}

TEST(ApiUtilsTest, testEasyInput_SubClassMissingInReleaseLibraries) {
  g_redex = new RedexContext();

  Scope scope = create_scope(true);
  api::ApiLevelsUtils api_utils(scope, std::getenv("api_utils_easy_input_path"),
                                21);

  const auto& types_to_framework_api = api_utils.get_types_to_framework_api();
  EXPECT_EQ(types_to_framework_api.size(), 2);

  auto a_framework = DexType::make_type("Landroid/util/ArrayMap;");
  auto a_release = DexType::make_type("Landroidx/ArrayMap;");
  EXPECT_EQ(types_to_framework_api.at(a_release).cls, a_framework);

  auto c_framework = DexType::make_type("Landroid/util/LongSparseArray;");
  auto c_release = DexType::make_type("Landroidx/LongSparseArray;");
  EXPECT_EQ(types_to_framework_api.at(c_release).cls, c_framework);
}

TEST(ApiUtilsTest, testEasyInput_MethodMissing) {
  g_redex = new RedexContext();

  Scope scope = create_scope(false);

  auto void_args = DexTypeList::make_type_list({});
  auto void_object = DexProto::make_proto(type::java_lang_Object(), void_args);

  auto a_release = DexType::make_type("Landroidx/ArrayMap;");
  auto method = static_cast<DexMethod*>(DexMethod::make_method(
      a_release, DexString::make_string("foo"), void_object));
  method->set_access(ACC_PUBLIC);
  method->set_virtual(true);
  method->set_external();
  method->set_code(assembler::ircode_from_string("((return-void))"));

  auto a_cls = type_class(a_release);
  a_cls->add_method(method);

  api::ApiLevelsUtils api_utils(scope, std::getenv("api_utils_easy_input_path"),
                                21);

  const auto& types_to_framework_api = api_utils.get_types_to_framework_api();
  EXPECT_EQ(types_to_framework_api.size(), 1);

  auto b_framework = DexType::make_type("Landroid/util/ArraySet;");
  auto b_release = DexType::make_type("Landroidx/ArraySet;");
  EXPECT_EQ(types_to_framework_api.at(b_release).cls, b_framework);
}
