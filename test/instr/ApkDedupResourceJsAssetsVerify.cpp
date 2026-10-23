/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApkResources.h"
#include "DedupResourceJsAssetsVerifyHelper.h"

TEST_F(PreVerify, ApkDedupResourceJsAssetsTest) {
  auto res_table = ResourcesArscFile(writable_resource("resources.arsc"));
  dedupresource_js_assets_preverify(classes, &res_table);
}

TEST_F(PostVerify, ApkDedupResourceJsAssetsTest) {
  auto res_table = ResourcesArscFile(writable_resource("resources.arsc"));
  dedupresource_js_assets_postverify(classes, &res_table);
}
