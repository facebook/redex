/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <gtest/gtest.h>
#include <string>

#include "DexInstruction.h"
#include "Match.h"
#include "VerifyUtil.h"

TEST_F(PostVerify, RenameClassesV2) {
  std::cout << "Loaded classes: " << classes.size() << std::endl ;
}
