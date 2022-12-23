/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BundleResources.h"
#include "ObfuscateResourcesVerifyHelper.h"

TEST_F(PreVerify, BundleObfuscateResourceTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  obfuscateresource_preverify(&res_table);
}

TEST_F(PostVerify, BundleObfuscateResourceTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  obfuscateresource_postverify(&res_table);

  auto icon_ids = res_table.get_res_ids_by_name("icon");
  EXPECT_EQ(icon_ids.size(), 1);
  auto files = res_table.get_files_by_rid(icon_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/c.png");

  auto themed_ids = res_table.get_res_ids_by_name("themed");
  EXPECT_EQ(themed_ids.size(), 1);
  files = res_table.get_files_by_rid(themed_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/h.xml");

  auto hex_or_file_ids = res_table.get_res_ids_by_name("hex_or_file");
  EXPECT_EQ(hex_or_file_ids.size(), 1);
  files = res_table.get_files_by_rid(hex_or_file_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/a.xml");
}
