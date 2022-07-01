/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CppUtil.h"

#include <gtest/gtest.h>

#include <algorithm>

void test_iterators(StringSplitterIterable ssi,
                    const std::vector<std::string_view>& expected) {
  auto it = expected.begin();
  for (auto&& v : ssi) {
    EXPECT_EQ(*it, v);
    ASSERT_NE(it, expected.end());
    it++;
  }
  EXPECT_EQ(it, expected.end());
}

TEST(CppUtilTest, testStringSplitter) {
  std::string str = "test splitting  by space";
  std::vector<std::string_view> expected{"test", "splitting", "", "by",
                                         "space"};
  test_iterators(split_string(str, " "), expected);
}

TEST(CppUtilTest, testSpaceInTheEnd) {
  std::string str = "test extra space in the end ";
  std::vector<std::string_view> expected{"test", "extra", "space", "in",
                                         "the",  "end",   ""};
  test_iterators(split_string(str, " "), expected);
}

TEST(CppUtilTest, testTwoSpacesInTheEnd) {
  std::string str = "test extra two spaces in the end  ";
  std::vector<std::string_view> expected{"test", "extra", "two", "spaces", "in",
                                         "the",  "end",   "",    ""};
  test_iterators(split_string(str, " "), expected);
}

TEST(CppUtilTest, testStringSplitterWith2CharDelimiter) {
  std::string str = "Hello world  test splitting  by two spaces";
  std::vector<std::string_view> expected{"Hello world", "test splitting",
                                         "by two spaces"};
  test_iterators(split_string(str, "  "), expected);
}

TEST(CppUtilTest, testDelimiterDoesNotExist) {
  std::string str = "testdelimiterdoesnotexist";
  std::vector<std::string_view> expected{"testdelimiterdoesnotexist"};
  test_iterators(split_string(str, " "), expected);
}

TEST(CppUtilTest, testDelimiterLongerThanString) {
  std::string str = "test";
  std::vector<std::string_view> expected{"test"};
  test_iterators(split_string(str, "testdelimiterlongerthanstring"), expected);
}

TEST(CppUtilTest, testStrEqDelim) {
  std::string str = "test";
  std::vector<std::string_view> expected{"", ""};
  test_iterators(split_string(str, "test"), expected);
}

TEST(CppUtilTest, testStrEmpty) {
  std::string str = "";
  std::vector<std::string_view> expected{""};
  test_iterators(split_string(str, "test"), expected);
}

TEST(CppUtilTest, testThreeColumnEmptyCSV) {
  std::string str = ",,";
  std::vector<std::string_view> expected{"", "", ""};
  test_iterators(split_string(str, ","), expected);
}
