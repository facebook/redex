/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "VerifyUtil.h"

TEST_F(PreVerify, BundleReachTest) {
  EXPECT_NE(nullptr,
            find_class_named(classes, "Lcom/fb/bundles/MainActivity;"));
  EXPECT_NE(nullptr,
            find_class_named(classes, "Lcom/fb/bundles/MyApplication;"));
  EXPECT_NE(nullptr, find_class_named(classes, "Lcom/fb/bundles/Unused;"));
}

TEST_F(PostVerify, BundleReachTest) {
  EXPECT_NE(nullptr,
            find_class_named(classes, "Lcom/fb/bundles/MainActivity;"));
  EXPECT_NE(nullptr,
            find_class_named(classes, "Lcom/fb/bundles/MyApplication;"));
  EXPECT_EQ(nullptr, find_class_named(classes, "Lcom/fb/bundles/Unused;"));
}
