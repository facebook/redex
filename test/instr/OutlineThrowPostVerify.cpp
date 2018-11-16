/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>

#include "DexInstruction.h"
#include "Match.h"
#include "VerifyUtil.h"

TEST_F(PostVerify, OutlineThrow) {
  std::cout << "Loaded classes: " << classes.size() << std::endl;
}
