/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApkResources.h"
#include "ObfuscateResourcesVerifyHelper.h"

TEST_F(PreVerify, ApkObfuscateResourceTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  obfuscateresource_preverify(&res_table);
}

TEST_F(PostVerify, ApkObfuscateResourceTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  obfuscateresource_postverify(&res_table);

  auto icon_ids = res_table.get_res_ids_by_name("icon");
  EXPECT_EQ(icon_ids.size(), 1);
  auto files = res_table.get_files_by_rid(icon_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "r/c.png");

  auto themed_ids = res_table.get_res_ids_by_name("themed");
  EXPECT_EQ(themed_ids.size(), 1);
  files = res_table.get_files_by_rid(themed_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "r/h.xml");

  auto hex_or_file_ids = res_table.get_res_ids_by_name("hex_or_file");
  EXPECT_EQ(hex_or_file_ids.size(), 1);
  files = res_table.get_files_by_rid(hex_or_file_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "r/a.xml");
}
