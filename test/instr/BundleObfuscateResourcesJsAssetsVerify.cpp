/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BundleResources.h"
#include "RedexResources.h"
#include "verify/VerifyUtil.h"

// Same as BundleObfuscateResourcesVerify but with --used-js-assets passing
// "keep_me_unused_color", which should prevent that name from being obfuscated.
TEST_F(PostVerify, BundleObfuscateResourceJsAssetsTest) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);

  // This name is normally obfuscated, but --used-js-assets should keep it.
  auto keep_me_unused_color_ids =
      res_table.get_res_ids_by_name("keep_me_unused_color");
  EXPECT_EQ(keep_me_unused_color_ids.size(), 1);

  // Other names that are normally removed should still be removed.
  std::vector<std::string> removed{"app_name",       "delay",
                                   "duplicate_name", "margin_top",
                                   "padding_right",  "padding_right"};
  for (const auto& s : removed) {
    auto ids = res_table.get_res_ids_by_name(s);
    EXPECT_EQ(ids.size(), 0) << "Name \"" << s << "\" should be removed!";
  }

  // One fewer obfuscated name since keep_me_unused_color is preserved.
  auto name_removed_ids = res_table.get_res_ids_by_name(RESOURCE_NAME_REMOVED);
  EXPECT_EQ(name_removed_ids.size(), 35);
}
