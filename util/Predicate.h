/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstring>

inline bool starts_with(const char* test, const char* prefix) {
  while (true) {
    if (*prefix == '\0') {
      return true;
    }
    if (*test != *prefix) {
      return false;
    }
    ++test;
    ++prefix;
  }
}

inline bool ends_with(const char* test, const char* suffix) {
  auto slen = strlen(suffix);
  auto tlen = strlen(test);
  if (slen > tlen) {
    return false;
  }
  test += (tlen - slen);
  while (*suffix) {
    if (*suffix != *test) {
      return false;
    }
    ++suffix;
    ++test;
  }
  return true;
}
