/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#if defined(__SSE4_2__) && defined(__linux__) && defined(__STRCMP_LESS__)
#include <gtest/gtest.h>

#include <cstring>
#include <ctime>
#include <stdlib.h>
#include <string>
#include <sys/time.h>

extern "C" bool strcmp_less(const char* str1, const char* str2);

unsigned long long get_time_in_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  unsigned long long microsec = tv.tv_usec;
  unsigned long long sec = tv.tv_sec;
  return microsec / 1000 + sec * 1000;
}

TEST(StrcmpLessPerfTest, Test1) {
  const int iter = 1000000000;
  const int len = 8;
  const char* strs_equal[] = {
      "Lcom/some/class/name:methodname",
      "Lcom/some/class/name:methodname",
      "A/x",
      "A/x",
      "1234567890",
      "1234567890",
      "this string is very long very long very long very long",
      "this string is very long very long very long very long"};
  const char* strs_less[] = {
      "Lcom/some/class/name:methodnam",
      "Lcom/some/class/name:methodname",
      "A/",
      "A/x",
      "123456789",
      "1234567890",
      "this string is very long very long very long very lon",
      "this string is very long very long very long very long"};
  const char* strs_greater[] = {
      "Lcom/some/class/name:methodname",
      "Lcom/some/class/name:methodnam",
      "A/x",
      "A/",
      "1234567890",
      "123456789",
      "this string is very long very long very long very long",
      "this string is very long very long very long very lon"};
  long long result1 = 0;
  long long result2 = 0;
  unsigned long long ts1 = get_time_in_ms();
  for (int i = 0; i < iter; i++) {
    for (int j = 0; j < len - 1; j = j + 2) {
      result1 += (int)(strcmp(strs_equal[j], strs_equal[j + 1]) < 0);
      result1 += (int)(strcmp(strs_less[j], strs_less[j + 1]) < 0);
      result1 += (int)(strcmp(strs_greater[j], strs_greater[j + 1]) < 0);
    }
  }
  unsigned long long ts2 = get_time_in_ms();
  for (int i = 0; i < iter; i++) {
    for (int j = 0; j < len - 1; j = j + 2) {
      result2 += (int)(strcmp_less(strs_equal[j], strs_equal[j + 1]));
      result2 += (int)(strcmp_less(strs_less[j], strs_less[j + 1]));
      result2 += (int)(strcmp_less(strs_greater[j], strs_greater[j + 1]));
    }
  }
  unsigned long long ts3 = get_time_in_ms();
  printf("Execution time (ms) strcmp: %llu strcmp_less: %llu\n", ts2 - ts1,
         ts3 - ts2);
  EXPECT_EQ(result1, result2);
}
#endif // defined(__SSE4_2__) && defined(__linux__) && defined(__STRCMP_LESS__)
