/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "verify/VerifyUtil.h"

#include "BundleResources.h"
#include "ResourcesInliningPass.h"
#include "ResourcesInliningPassVerifyImpl.h"

TEST_F(PreVerify, ResourcesInliningPassTest) {
  const auto& resource_pb_file = resources.at("base/resources.pb");
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  resource_inlining_PreVerify(&res_table);
}

TEST_F(PostVerify, ResourcesInliningPassTest_DexPatching) {
  auto cls = find_class_named(classes, "Lcom/fb/resources/MainActivity;");
  resource_inlining_PostVerify(cls);
}
