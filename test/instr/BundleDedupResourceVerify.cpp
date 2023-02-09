/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BundleResources.h"
#include "DedupResourceVerifyHelper.h"

TEST_F(PreVerify, BundleDedupResourceTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  dedupresource_preverify(classes, &res_table);
  auto same_styleable_a_ids = res_table.get_res_ids_by_name("SameStyleableA");
  auto same_styleable_b_ids = res_table.get_res_ids_by_name("SameStyleableB");
  EXPECT_EQ(same_styleable_a_ids.size(), 1);
  EXPECT_EQ(same_styleable_b_ids.size(), 1);
}

TEST_F(PostVerify, BundleDedupResourceTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  dedupresource_postverify(classes, &res_table);
  auto same_styleable_a_ids = res_table.get_res_ids_by_name("SameStyleableA");
  auto same_styleable_b_ids = res_table.get_res_ids_by_name("SameStyleableB");
  EXPECT_EQ(same_styleable_a_ids.size(), 1);
  EXPECT_EQ(same_styleable_b_ids.size(), 1);
}
