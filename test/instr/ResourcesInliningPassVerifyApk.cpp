/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "verify/VerifyUtil.h"

#include "ApkResources.h"
#include "ResourcesInliningPass.h"
#include "ResourcesInliningPassVerifyImpl.h"

TEST_F(PreVerify, ResourcesInliningPassTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  resource_inlining_PreVerify(&res_table);
}

TEST_F(PostVerify, ResourcesInliningPassTest_DexPatching) {
  auto cls = find_class_named(classes, "Lcom/fb/resources/MainActivity;");
  resource_inlining_PostVerify(cls);
}
