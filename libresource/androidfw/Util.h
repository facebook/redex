/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef _FB_UTIL_REIMPLEMENTATION
#define _FB_UTIL_REIMPLEMENTATION

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>


namespace android {
namespace util {

inline std::vector<std::string> SplitAndLowercase(const std::string& str, char sep) {
  std::vector<std::string> result;
  std::stringstream ss(str);
  std::string part;
  while (std::getline(ss, part, sep)) {
    std::transform(part.begin(), part.end(), part.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    result.push_back(part);
  }
  return result;
}

} // namespace util
} // namespace android

#endif // _FB_UTIL_REIMPLEMENTATION
