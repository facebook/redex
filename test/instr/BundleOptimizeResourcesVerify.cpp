/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BundleResources.h"
#include "OptimizeResourcesVerifyImpl.h"

TEST_F(PreVerify, BundleOptimizeResourcesTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  preverify_impl(classes, &res_table);
}

TEST_F(PostVerify, BundleOptimizeResourcesTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  postverify_impl(classes, &res_table);
}
