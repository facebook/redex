/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApkResources.h"
#include "RedexResources.h"
#include "SplitResourceTablesVerifyImpl.h"
#include "androidfw/ResourceTypes.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, VerifyNewTypeCreated) {
  auto resources_path = resources["resources.arsc"];
  ResourcesArscFile arsc_file(resources_path);
  auto& table_snapshot = arsc_file.get_table_snapshot();
  // Actual lookup, data type validation will differ for .apk. Do that in the
  // callback.
  auto value_getter = [&](uint32_t id) {
    std::vector<std::string> result;
    std::vector<android::Res_value> out_vals;
    table_snapshot.collect_resource_values(id, &out_vals);
    for (const auto& val : out_vals) {
      if (val.dataType == android::Res_value::TYPE_STRING) {
        result.emplace_back(table_snapshot.get_global_string(dtohl(val.data)));
      }
    }
    return result;
  };
  // Common validation about type id creation, id compaction and string value
  // checks that are common between .apk inputs and .aab inputs.
  postverify_impl(classes, value_getter, &arsc_file);
}
