/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <string>
#include <gtest/gtest.h>

#include "RedexResources.h"

std::unordered_set<std::string> extract_classes_from_native_lib(
    const std::string& lib_contents);

TEST(ExtractNativeTest, empty) {
  std::string over(700, 'L');
  auto overset = extract_classes_from_native_lib(over);
  EXPECT_EQ(overset.size(), 2);
}
