/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BundleResources.h"
#include "ResourceValueMergingPassVerifyImpl.h"
#include "verify/VerifyUtil.h"

TEST_F(PreVerify, BundleResourceValueMergingPassTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  resource_value_merging_PreVerify(&res_table);
}

TEST_F(PostVerify, BundleResourceValueMergingPassTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  resource_value_merging_PostVerify(&res_table);
}
