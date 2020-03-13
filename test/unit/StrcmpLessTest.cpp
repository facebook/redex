/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#if defined(__SSE4_2__) && defined(__linux__) && defined(__STRCMP_LESS__)
#include <gtest/gtest.h>

#include <cstring>
#include <stdlib.h>
#include <string>

extern "C" bool strcmp_less(const char* str1, const char* str2);

TEST(StrcmpLessTest, Test1) {
  const char* str1 = "a";
  const char* str2 = "a";
  EXPECT_FALSE(strcmp_less(str1, str2));
}

TEST(StrcmpLessTest, Test2) {
  const char* str1 = "a";
  const char* str2 = "b";
  EXPECT_TRUE(strcmp_less(str1, str2));
}

TEST(StrcmpLessTest, Test3) {
  const char* str1 = "b";
  const char* str2 = "a";
  EXPECT_FALSE(strcmp_less(str1, str2));
}

TEST(StrcmpLessTest, Test4) {
  const char* str1 = "abcd";
  const char* str2 = "abcd";
  EXPECT_FALSE(strcmp_less(str1, str2));
}

TEST(StrcmpLessTest, Test5) {
  const char* str1 = "abcd";
  const char* str2 = "abce";
  EXPECT_TRUE(strcmp_less(str1, str2));
}

TEST(StrcmpLessTest, Test6) {
  const char* str1 = "abce";
  const char* str2 = "abcd";
  EXPECT_FALSE(strcmp_less(str1, str2));
}

TEST(StrcmpLessTest, Test7) {
  const char* str1 = "abcd";
  const char* str2 = "abcde";
  EXPECT_TRUE(strcmp_less(str1, str2));
}

TEST(StrcmpLessTest, Test8) {
  const char* str1 = "abcde";
  const char* str2 = "abcd";
  EXPECT_FALSE(strcmp_less(str1, str2));
}

TEST(StrcmpLessTest, Test9) {
  std::string str1;
  std::string str2;
  const int min_str_len = 1;
  const int max_str_len = 100;
  const int loop_iter = 10000;
  for (int i = 0; i < loop_iter; i++) {
    str1.clear();
    str2.clear();
    int str1_len = rand() % max_str_len + min_str_len;
    int str2_len = rand() % max_str_len + min_str_len;
    for (int j = 0; j < str1_len; j++) {
      str1.push_back(static_cast<char>(rand()));
    }
    for (int j = 0; j < str2_len; j++) {
      str2.push_back(static_cast<char>(rand()));
    }
    bool result1 = strcmp_less(str1.c_str(), str2.c_str());
    bool result2 = (strcmp(str1.c_str(), str2.c_str()) < 0);
    EXPECT_EQ(result1, result2);
  }
}

// str1 == str2
TEST(StrcmpLessTest, Test10) {
  std::string str1;
  std::string str2;
  const int min_str_len = 1;
  const int max_str_len = 100;
  const int loop_iter = 10000;
  for (int i = 0; i < loop_iter; i++) {
    str1.clear();
    str2.clear();
    int len = rand() % max_str_len + min_str_len;
    for (int j = 0; j < len; j++) {
      char c = static_cast<char>(rand());
      str1.push_back(c);
      str2.push_back(c);
    }
    EXPECT_FALSE(strcmp_less(str1.c_str(), str2.c_str()));
  }
}

// str1 < str2
TEST(StrcmpLessTest, Test11) {
  std::string str1;
  std::string str2;
  const int min_str_len = 1;
  const int max_str_len = 100;
  const int loop_iter = 10000;
  for (int i = 0; i < loop_iter; i++) {
    str1.clear();
    str2.clear();
    int len = rand() % max_str_len + min_str_len;
    for (int j = 0; j < len; j++) {
      int min_char = static_cast<int>('b');
      int max_char = static_cast<int>('y');
      int range = max_char - min_char + 1;
      char c = static_cast<char>(rand() % range + min_char);
      str1.push_back(c);
      str2.push_back(c);
    }
    str2[len - 1] = static_cast<char>(str2[len - 1] + 1);
    EXPECT_TRUE(strcmp_less(str1.c_str(), str2.c_str()));
  }
}

// str1 > str2
TEST(StrcmpLessTest, Test12) {
  std::string str1;
  std::string str2;
  const int min_str_len = 1;
  const int max_str_len = 100;
  const int loop_iter = 10000;
  for (int i = 0; i < loop_iter; i++) {
    str1.clear();
    str2.clear();
    int len = rand() % max_str_len + min_str_len;
    for (int j = 0; j < len; j++) {
      int min_char = static_cast<int>('b');
      int max_char = static_cast<int>('y');
      int range = max_char - min_char + 1;
      char c = static_cast<char>(rand() % range + min_char);
      str1.push_back(c);
      str2.push_back(c);
    }
    str2[len - 1] = static_cast<char>(str2[len - 1] - 1);
    EXPECT_FALSE(strcmp_less(str1.c_str(), str2.c_str()));
  }
}
#endif // defined(__SSE4_2__) && defined(__linux__) && defined(__STRCMP_LESS__)
