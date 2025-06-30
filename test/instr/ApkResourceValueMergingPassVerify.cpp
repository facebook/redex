/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApkResources.h"
#include "ResourceValueMergingPassVerifyImpl.h"
#include "verify/VerifyUtil.h"

TEST_F(PreVerify, ApkResourceValueMergingPassTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  resource_value_merging_PreVerify(&res_table);
}

TEST_F(PostVerify, ApkResourceValueMergingPassTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  resource_value_merging_PostVerify(&res_table);
}
