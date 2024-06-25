/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "verify/VerifyUtil.h"

#include "ApkResources.h"

TEST_F(PreVerify, ResourcesInliningPassTest) {
  const auto& resource_arsc_file = resources.at("resources.arsc");
  auto res_table = ResourcesArscFile(resource_arsc_file);
  EXPECT_THAT(res_table.sorted_res_ids,
              testing::ElementsAre(
                  /* bool */
                  0x7f010000,
                  /* color */
                  0x7f020000,
                  0x7f020001,
                  0x7f020002,
                  0x7f020003,
                  /* dimen */
                  0x7f030000,
                  0x7f030001,
                  /* integer */
                  0x7f040000,
                  /* layout */
                  0x7f050000,
                  0x7f050001,
                  0x7f050002,
                  /* string */
                  0x7f060000));
}

TEST_F(PostVerify, ResourcesInliningPassTest) {}
