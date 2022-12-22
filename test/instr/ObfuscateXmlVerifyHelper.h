/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <functional>
#include <gtest/gtest.h>
#include <set>

#include "RedexResources.h"
#include "verify/VerifyUtil.h"

inline void verify_kept_xml_attributes(
    const ResourceFiles& resources,
    const std::function<std::set<std::string>(const std::string&)>&
        attribute_getter) {
  auto has_keeps_path = resources.at("res/layout/activity_main.xml");
  auto set1 = attribute_getter(has_keeps_path);
  EXPECT_EQ(set1.count("a_boolean"), 1);
  EXPECT_EQ(set1.count("fancy_effects"), 1);

  auto no_keeps_path = resources.at("res/layout/themed.xml");
  auto set2 = attribute_getter(no_keeps_path);
  // For this file, all attribute names should have been collapsed to a single
  // empty string.
  EXPECT_EQ(set2.size(), 1);
  EXPECT_EQ(set2.count(""), 1);
}
