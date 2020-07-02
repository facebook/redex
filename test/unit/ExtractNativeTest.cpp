/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "RedexResources.h"

TEST(ExtractNativeTest, empty) {
  std::string over(700, 'L');
  auto overset = extract_classes_from_native_lib(over);
  EXPECT_EQ(overset.size(), 2);
}
