/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApkResources.h"
#include "DedupResourceVerifyHelper.h"

namespace {

const std::vector<std::string> KEPT_FILE_PATHS = {
    "res/color-night-v8/hex_or_file.xml",
    "res/drawable-mdpi-v4/icon.png",
    "res/drawable-mdpi-v4/prickly.png",
    "res/layout/activity_main.xml",
    "res/layout/themed.xml",
    "res/layout/also_red_button.xml",
};

const std::vector<std::string> REMOVED_FILE_PATHS = {
    "res/color/hex_or_file2.xml",
    "res/drawable-mdpi-v4/x_icon.png",
    "res/drawable-mdpi-v4/x_prickly.png",
    "res/layout/red_button.xml",
};

} // namespace

TEST_F(PreVerify, ApkDedupResourceTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  dedupresource_preverify(classes, &res_table);
}

TEST_F(PostVerify, ApkDedupResourceTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  dedupresource_postverify(classes, &res_table);
  // Perform post validation only relevant to .apk files.
  auto& pool = res_table.get_table_snapshot().get_global_strings();
  std::unordered_set<std::string> global_strings;
  for (int i = 0; i < pool.size(); i++) {
    global_strings.emplace(apk::get_string_from_pool(pool, i));
  }
  for (const auto& s : KEPT_FILE_PATHS) {
    EXPECT_EQ(global_strings.count(s), 1)
        << "Global string pool should contain string " << s.c_str();
  }
  for (const auto& s : REMOVED_FILE_PATHS) {
    EXPECT_EQ(global_strings.count(s), 0)
        << "Global string pool should NOT contain string " << s.c_str();
  }
}
