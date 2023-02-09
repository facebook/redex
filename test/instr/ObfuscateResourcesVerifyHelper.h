/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "RedexResources.h"
#include "verify/VerifyUtil.h"

inline void obfuscateresource_preverify(ResourceTableFile* res_table) {
  auto margin_top_ids = res_table->get_res_ids_by_name("margin_top");
  auto padding_right_ids = res_table->get_res_ids_by_name("padding_right");
  auto unused_dimen_1_ids = res_table->get_res_ids_by_name("unused_dimen_1");
  auto unused_dimen_2_ids = res_table->get_res_ids_by_name("unused_dimen_2");
  auto keep_me_unused_color_ids =
      res_table->get_res_ids_by_name("keep_me_unused_color");
  auto welcome_text_size_ids =
      res_table->get_res_ids_by_name("welcome_text_size");
  auto app_name_ids = res_table->get_res_ids_by_name("app_name");
  auto name_removed_ids = res_table->get_res_ids_by_name(RESOURCE_NAME_REMOVED);
  auto duplicate_name_ids = res_table->get_res_ids_by_name("duplicate_name");
  EXPECT_EQ(margin_top_ids.size(), 1);
  EXPECT_EQ(padding_right_ids.size(), 1);
  EXPECT_EQ(unused_dimen_1_ids.size(), 1);
  EXPECT_EQ(unused_dimen_2_ids.size(), 1);
  EXPECT_EQ(keep_me_unused_color_ids.size(), 1);
  EXPECT_EQ(app_name_ids.size(), 1);
  EXPECT_EQ(welcome_text_size_ids.size(), 1);
  EXPECT_EQ(duplicate_name_ids.size(), 3);
  EXPECT_EQ(name_removed_ids.size(), 0);

  auto icon_ids = res_table->get_res_ids_by_name("icon");
  EXPECT_EQ(icon_ids.size(), 1);
  auto files = res_table->get_files_by_rid(icon_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/drawable-mdpi-v4/icon.png");

  auto x_prickly_ids = res_table->get_res_ids_by_name("x_prickly");
  EXPECT_EQ(x_prickly_ids.size(), 1);
  files = res_table->get_files_by_rid(x_prickly_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/drawable-mdpi-v4/x_prickly.png");

  auto activity_main_ids = res_table->get_res_ids_by_name("activity_main");
  EXPECT_EQ(activity_main_ids.size(), 1);
  files = res_table->get_files_by_rid(activity_main_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/layout/activity_main.xml");

  auto themed_ids = res_table->get_res_ids_by_name("themed");
  EXPECT_EQ(themed_ids.size(), 1);
  files = res_table->get_files_by_rid(themed_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/layout/themed.xml");

  auto hex_or_file_ids = res_table->get_res_ids_by_name("hex_or_file");
  EXPECT_EQ(hex_or_file_ids.size(), 1);
  files = res_table->get_files_by_rid(hex_or_file_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/color-night-v8/hex_or_file.xml");
}

inline void obfuscateresource_postverify(ResourceTableFile* res_table) {
  auto margin_top_ids = res_table->get_res_ids_by_name("margin_top");
  auto padding_right_ids = res_table->get_res_ids_by_name("padding_right");
  auto unused_dimen_1_ids = res_table->get_res_ids_by_name("unused_dimen_1");
  auto unused_dimen_2_ids = res_table->get_res_ids_by_name("unused_dimen_2");
  auto keep_me_unused_color_ids =
      res_table->get_res_ids_by_name("keep_me_unused_color");
  auto welcome_text_size_ids =
      res_table->get_res_ids_by_name("welcome_text_size");
  auto app_name_ids = res_table->get_res_ids_by_name("app_name");
  auto name_removed_ids = res_table->get_res_ids_by_name(RESOURCE_NAME_REMOVED);
  auto duplicate_name_ids = res_table->get_res_ids_by_name("duplicate_name");
  EXPECT_EQ(margin_top_ids.size(), 0);
  EXPECT_EQ(padding_right_ids.size(), 0);
  EXPECT_EQ(unused_dimen_1_ids.size(), 1);
  EXPECT_EQ(unused_dimen_2_ids.size(), 1);
  EXPECT_EQ(keep_me_unused_color_ids.size(), 0);
  EXPECT_EQ(app_name_ids.size(), 1);
  EXPECT_EQ(welcome_text_size_ids.size(), 1);
  EXPECT_EQ(duplicate_name_ids.size(), 1);
  EXPECT_EQ(name_removed_ids.size(), 27);

  auto duplicate_name_id = res_table->get_res_ids_by_name("duplicate_name")[0];
  std::unordered_set<std::string> types = {"string"};
  auto string_type = res_table->get_types_by_name(types);
  EXPECT_EQ(string_type.size(), 1);
  EXPECT_EQ(duplicate_name_id & TYPE_MASK_BIT, *(string_type.begin()));

  auto x_prickly_ids = res_table->get_res_ids_by_name("x_prickly");
  EXPECT_EQ(x_prickly_ids.size(), 1);
  auto files = res_table->get_files_by_rid(x_prickly_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/drawable-mdpi-v4/x_prickly.png");

  auto activity_main_ids = res_table->get_res_ids_by_name("activity_main");
  EXPECT_EQ(activity_main_ids.size(), 1);
  files = res_table->get_files_by_rid(activity_main_ids[0]);
  EXPECT_EQ(files.size(), 1);
  EXPECT_EQ(*files.begin(), "res/layout/activity_main.xml");

  // id name used in .xml layout
  auto welcome_view_ids = res_table->get_res_ids_by_name("welcome_view");
  EXPECT_EQ(welcome_view_ids.size(), 1);
  // id name that should be elligible for obfuscation
  auto delay_ids = res_table->get_res_ids_by_name("delay");
  EXPECT_EQ(delay_ids.size(), 0);
}
