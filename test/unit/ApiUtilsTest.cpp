/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApiLevelsUtils.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "ScopeHelper.h"

namespace {

std::vector<DexClass*> create_scope(bool extra_class) {
  std::vector<DexClass*> scope = create_empty_scope();
  auto obj_t = get_object_type();

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_internal_class(a_t, obj_t, {});
  scope.push_back(a_cls);

  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_internal_class(b_t, obj_t, {});
  scope.push_back(b_cls);

  auto c_t = DexType::make_type("LC;");
  auto c_cls = create_internal_class(c_t, a_t, {});
  scope.push_back(c_cls);

  if (extra_class) {
    auto d_t = DexType::make_type("LD;");
    auto d_cls = create_internal_class(d_t, b_t, {});
    scope.push_back(d_cls);
  }

  return scope;
}

} // namespace

TEST(ApiUtilsTest, testEasyInputFormat) {
  g_redex = new RedexContext();

  Scope scope = create_scope(false);
  api::ApiLevelsUtils api_utils(scope,
                                std::getenv("api_utils_easy_input_path"));

  const auto& types_to_framework_api = api_utils.get_types_to_framework_api();
  EXPECT_EQ(types_to_framework_api.size(), 3);

  auto a_t = DexType::make_type("LA;");
  EXPECT_EQ(types_to_framework_api.at(a_t).methods.size(), 2);

  auto b_t = DexType::make_type("LB;");
  EXPECT_EQ(types_to_framework_api.at(b_t).methods.size(), 2);

  auto c_t = DexType::make_type("LC;");
  EXPECT_EQ(types_to_framework_api.at(c_t).methods.size(), 0);

  delete g_redex;
}

TEST(ApiUtilsTest, testSubClassMissingInputFormat) {
  g_redex = new RedexContext();

  Scope scope = create_scope(true);
  api::ApiLevelsUtils api_utils(scope,
                                std::getenv("api_utils_easy_input_path"));

  const auto& types_to_framework_api = api_utils.get_types_to_framework_api();
  EXPECT_EQ(types_to_framework_api.size(), 2);

  delete g_redex;
}
