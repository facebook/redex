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
  std::unordered_map<uint32_t, resources::InlinableValue> inlinable =
      res_table.get_inlinable_resource_values();

  EXPECT_TRUE(inlinable.find(0x7f010000) != inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f020000) != inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f020001) != inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f020002) != inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f020003) == inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f030000) == inlinable.end());
  EXPECT_TRUE(inlinable.find(0x7f030001) == inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f040000) != inlinable.end());

  EXPECT_TRUE(inlinable.find(0x7f060000) != inlinable.end());

  auto val = inlinable.at(0x7f010000);
  EXPECT_EQ(val.type, android::Res_value::TYPE_INT_BOOLEAN);
  EXPECT_TRUE(val.bool_value);

  val = inlinable.at(0x7f020000);
  EXPECT_EQ(val.type, android::Res_value::TYPE_INT_COLOR_RGB8);
  EXPECT_EQ(val.uint_value, 0xff673ab7);

  val = inlinable.at(0x7f020001);
  EXPECT_GE(val.type, android::Res_value::TYPE_FIRST_COLOR_INT);
  EXPECT_LE(val.type, android::Res_value::TYPE_LAST_COLOR_INT);
  EXPECT_EQ(val.uint_value, 0xffff0000);

  val = inlinable.at(0x7f020002);
  EXPECT_EQ(val.type, android::Res_value::TYPE_INT_COLOR_RGB8);
  EXPECT_EQ(val.uint_value, 0xff673ab7);

  val = inlinable.at(0x7f040000);
  EXPECT_GE(val.type, android::Res_value::TYPE_FIRST_INT);
  EXPECT_LE(val.type, android::Res_value::TYPE_INT_HEX);
  EXPECT_EQ(val.uint_value, 3);

  val = inlinable.at(0x7f060000);
  EXPECT_EQ(val.type, android::Res_value::TYPE_STRING);
  EXPECT_EQ(val.string_value.substr(0, 6), "Hello,");
}
