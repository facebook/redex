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
  auto res_table = ResourcesArscFile(writable_resource("resources.arsc"));
  preverify_nullify_impl(classes, &res_table);
}

TEST_F(PostVerify, ApkOptimizeResourcesTest) {
  auto res_table = ResourcesArscFile(writable_resource("resources.arsc"));
  postverify_nullify_impl(classes, &res_table);
  // Perform post validation only relevant to .apk files.
  apk_postverify_nullify_impl(&res_table);
}
