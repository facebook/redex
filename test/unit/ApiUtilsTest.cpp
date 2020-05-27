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

  auto make_cls = [&](const char* name, DexType* super_cls) {
    auto t = DexType::make_type(name);
    auto cls = create_internal_class(t, super_cls, {});
    scope.push_back(cls);
    return cls;
  };

  auto a_cls = make_cls("Landroidx/ArrayMap;", obj_t);
  DexType* b_parent = obj_t;
  if (add_parent) {
    auto p_cls = make_cls("Landroidx/ArraySetParentClass;", obj_t);
    b_parent = p_cls->get_type();
  }
  make_cls("Landroidx/ArraySet;", b_parent);
  make_cls("Landroidx/LongSparseArray;", a_cls->get_type());
  make_cls("Landroidx/FragmentContainer;", obj_t);
  make_cls("Landroid/view/View;", obj_t);

  return scope;
}

void add_usage(Scope* scope, DexMethodRef* mref) {
  auto obj_t = type::java_lang_Object();
  auto new_t = DexType::make_type("LUsage;");
  auto new_cls = create_internal_class(new_t, obj_t, {});
  scope->push_back(new_cls);

  std::string code_str = R"(
    (method (private) "LUsage;.bar:()V"
      (
        (new-instance "Landroidx/ArrayMap;")
        (move-result-pseudo-object v0)
        (invoke-virtual (v0) ")";
  code_str += SHOW(mref);
  code_str += R"(")
        (move-result-object v0)
        (return-void)
      )
    )
  )";
  std::cout << code_str << std::endl;
  auto method = assembler::method_from_string(code_str);
  new_cls->add_method(method);
}

} // namespace

TEST(ApiUtilsTest, testParseInputFormat) {
  g_redex = new RedexContext();

  Scope scope = create_scope(false);
  api::ApiLevelsUtils api_utils(scope, std::getenv("api_utils_easy_input_path"),
                                21);
  const auto& framework_cls_to_api = api_utils.get_framework_classes();
  EXPECT_EQ(framework_cls_to_api.size(), 6);

  auto a_t = DexType::make_type("Landroid/util/ArrayMap;");
  EXPECT_EQ(framework_cls_to_api.at(a_t).mrefs_info.size(), 2);
  EXPECT_EQ(framework_cls_to_api.at(a_t).frefs_info.size(), 0);

  auto b_t = DexType::make_type("Landroid/util/ArraySet;");
  EXPECT_EQ(framework_cls_to_api.at(b_t).mrefs_info.size(), 2);

  auto c_t = DexType::make_type("Landroid/util/LongSparseArray;");
  EXPECT_EQ(framework_cls_to_api.at(c_t).mrefs_info.size(), 0);

  auto d_t = DexType::make_type("Landroid/app/FragmentContainer;");
  EXPECT_EQ(framework_cls_to_api.at(d_t).mrefs_info.size(), 0);

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

TEST(ApiUtilsTest, testEasyInput_MethodMissingButNotTruePrivate) {
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
  EXPECT_EQ(types_to_framework_api.size(), 3);

  auto b_framework = DexType::make_type("Landroid/util/ArraySet;");
  auto b_release = DexType::make_type("Landroidx/ArraySet;");
  EXPECT_EQ(types_to_framework_api.at(b_release).cls, b_framework);
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

  // We need to actually use the method outside of the class, for it to be
  // considered missing.
  add_usage(&scope, method);

  api::ApiLevelsUtils api_utils(scope, std::getenv("api_utils_easy_input_path"),
                                21);

  const auto& types_to_framework_api = api_utils.get_types_to_framework_api();
  EXPECT_EQ(types_to_framework_api.size(), 1);

  auto b_framework = DexType::make_type("Landroid/util/ArraySet;");
  auto b_release = DexType::make_type("Landroidx/ArraySet;");
  EXPECT_EQ(types_to_framework_api.at(b_release).cls, b_framework);
}

TEST(ApiUtilsTest, testHasMethod) {
  g_redex = new RedexContext();

  Scope scope = create_scope(false);

  api::ApiLevelsUtils api_utils(scope, std::getenv("api_utils_easy_input_path"),
                                21);
  const auto& framework_cls_to_api = api_utils.get_framework_classes();
  EXPECT_EQ(framework_cls_to_api.size(), 6);

  auto android_view = DexType::make_type("Landroid/view/View;");

  EXPECT_EQ(framework_cls_to_api.at(android_view).mrefs_info.size(), 1);
  EXPECT_EQ(framework_cls_to_api.at(android_view).frefs_info.size(), 0);

  const auto& api = framework_cls_to_api.at(android_view);
  auto void_args = DexTypeList::make_type_list({});
  auto void_empty = DexProto::make_proto(type::_void(), void_args);
  EXPECT_TRUE(api.has_method("clearFocus", void_empty, ACC_PUBLIC));
  EXPECT_FALSE(api.has_method("joJo", void_empty, ACC_PUBLIC));

  auto api_file =
      boost::optional<std::string>(std::getenv("api_utils_easy_input_path"));
  api::AndroidSDK sdk(api_file);
  auto method = static_cast<DexMethod*>(DexMethod::make_method(
      android_view, DexString::make_string("clearFocus"), void_empty));
  method->set_access(ACC_PUBLIC);
  method->set_virtual(true);
  method->set_external();
  method->set_code(assembler::ircode_from_string("((return-void))"));
  auto a_cls = type_class(android_view);
  a_cls->add_method(method);

  EXPECT_TRUE(sdk.has_method(method));
}
