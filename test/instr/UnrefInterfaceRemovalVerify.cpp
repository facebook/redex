/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Resolver.h"
#include "Show.h"
#include "VerifyUtil.h"

TEST_F(PostVerify, SimpleUnusedCase) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/A;");
  ASSERT_NE(cls_a, nullptr);
  auto itfs = cls_a->get_interfaces()->get_type_list();
  ASSERT_TRUE(itfs.empty());
}
