/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstring>

inline bool starts_with(const char* test, const char* prefix) {
  return strncmp(test, prefix, strlen(prefix)) == 0;
}

inline bool ends_with(const char* test, const char* suffix) {
  auto slen = strlen(suffix);
  auto tlen = strlen(test);
  if (slen > tlen) {
    return false;
  }
  return strncmp(test + tlen - slen, suffix, slen) == 0;
}
