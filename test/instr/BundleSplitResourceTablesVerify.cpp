/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BundleResources.h"
#include "RedexResources.h"
#include "SplitResourceTablesVerifyImpl.h"
#include "androidfw/ResourceTypes.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, VerifyNewTypeCreated) {
  const auto& resource_pb_file = resources["base/resources.pb"];
  auto res_table = ResourcesPbFile();
  res_table.collect_resource_data_for_file(resource_pb_file);
  // NOTE: This is OK for the validadation being performed today, as all the
  // string values expected are really file paths. Change the API call here if
  // the test data gets altered.
  auto value_getter = [&](uint32_t id) {
    return res_table.get_files_by_rid(id);
  };
  // Common validation about type id creation, id compaction and string value
  // checks that are common between .apk inputs and .aab inputs.
  postverify_impl(classes, value_getter, &res_table);
}
