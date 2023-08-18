/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApkResources.h"
#include "OptimizeResourcesVerifyImpl.h"

TEST_F(PreVerify, ApkOptimizeResourcesTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  preverify_nullify_impl(classes, &res_table);
}

TEST_F(PostVerify, ApkOptimizeResourcesTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  postverify_nullify_impl(classes, &res_table);
  // Perform post validation only relevant to .apk files.
  apk_postverify_nullify_impl(&res_table);
}
