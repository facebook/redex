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
  std::string str;
  std::vector<std::string_view> expected{""};
  test_iterators(split_string(str, "test"), expected);
}

TEST(CppUtilTest, testThreeColumnEmptyCSV) {
  std::string str = ",,";
  std::vector<std::string_view> expected{"", "", ""};
  test_iterators(split_string(str, ","), expected);
}

namespace {

template <typename T>
testing::AssertionResult is_aligned(const T* ptr, size_t alignment) {
  uintptr_t ptr_val = reinterpret_cast<uintptr_t>(ptr);
  if (ptr_val % alignment == 0) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure()
         << std::hex << ptr_val << " is not aligned by " << std::dec
         << alignment;
}

template <typename T>
T* gen(uintptr_t base, size_t offset) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return reinterpret_cast<T*>(base + offset);
}

} // namespace

TEST(CppUtilTest, align_ptr_void_ptr) {
  constexpr uintptr_t base = 0x1000;

  EXPECT_TRUE(is_aligned(align_ptr<1>(gen<void*>(base, 0)), 1));
  EXPECT_TRUE(is_aligned(align_ptr<1>(gen<void*>(base, 1)), 1));
  EXPECT_TRUE(is_aligned(align_ptr<1>(gen<void*>(base, 2)), 1));

  EXPECT_TRUE(is_aligned(align_ptr<2>(gen<void*>(base, 0)), 2));
  EXPECT_TRUE(is_aligned(align_ptr<2>(gen<void*>(base, 1)), 2));
  EXPECT_TRUE(is_aligned(align_ptr<2>(gen<void*>(base, 2)), 2));

  EXPECT_TRUE(is_aligned(align_ptr<4>(gen<void*>(base, 0)), 4));
  EXPECT_TRUE(is_aligned(align_ptr<4>(gen<void*>(base, 1)), 4));
  EXPECT_TRUE(is_aligned(align_ptr<4>(gen<void*>(base, 2)), 4));
  EXPECT_TRUE(is_aligned(align_ptr<4>(gen<void*>(base, 3)), 4));

  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<void*>(base, 0)), 8));
  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<void*>(base, 1)), 8));
  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<void*>(base, 2)), 8));
  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<void*>(base, 3)), 8));
  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<void*>(base, 4)), 8));
  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<void*>(base, 5)), 8));
  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<void*>(base, 6)), 8));
  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<void*>(base, 7)), 8));
}

TEST(CppUtilTest, align_ptr_other_ptr) {
  constexpr uintptr_t base = 0x1000;

  EXPECT_TRUE(is_aligned(align_ptr<2>(gen<uint16_t*>(base, 0)), 2));
  EXPECT_TRUE(is_aligned(align_ptr<2>(gen<uint16_t*>(base, 1)), 2));
  EXPECT_TRUE(is_aligned(align_ptr<2>(gen<uint16_t*>(base, 2)), 2));

  EXPECT_TRUE(is_aligned(align_ptr<4>(gen<uint32_t*>(base, 0)), 4));
  EXPECT_TRUE(is_aligned(align_ptr<4>(gen<uint32_t*>(base, 1)), 4));
  EXPECT_TRUE(is_aligned(align_ptr<4>(gen<uint32_t*>(base, 2)), 4));
  EXPECT_TRUE(is_aligned(align_ptr<4>(gen<uint32_t*>(base, 3)), 4));

  EXPECT_TRUE(is_aligned(align_ptr<8>(gen<std::vector<char>*>(base, 1)), 8));
}
