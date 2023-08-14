/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <gtest/gtest.h>
#include <unordered_map>
#include <vector>

#include "verify/VerifyUtil.h"

// .apk format and .aab format will be munged into a simply data structure to
// run
struct Element {
  std::string name;
  std::unordered_map<std::string, std::string> string_attributes;
};

inline void verify_xml_element_attributes(std::vector<Element> elements) {
  EXPECT_EQ(elements.size(), 4);

  auto linear_layout = elements.at(0);
  EXPECT_STREQ(linear_layout.name.c_str(), "LinearLayout");
  EXPECT_EQ(linear_layout.string_attributes.count("class"), 0);

  auto optimized_view = elements.at(1);
  EXPECT_EQ(optimized_view.name, "view");
  EXPECT_EQ(optimized_view.string_attributes["class"], "android.view.View");

  auto optimized_viewstub = elements.at(2);
  EXPECT_EQ(optimized_viewstub.name, "view");
  EXPECT_EQ(optimized_viewstub.string_attributes["class"],
            "android.view.ViewStub");

  // Should not be modified because there is already a conflicting class="derp"
  // attribute.
  auto unoptimized_view = elements.at(3);
  EXPECT_EQ(unoptimized_view.name, "View"); // capital View
  EXPECT_EQ(unoptimized_view.string_attributes["class"], "derp");
}
