/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Show.h"
#include "VerifyUtil.h"

TEST_F(PostVerify, DoNotInlineAnnotated) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redexinline/MethodInlineTest;");
  ASSERT_NE(nullptr, cls);

  auto m1 = find_vmethod_named(*cls, "getHello");
  ASSERT_EQ(nullptr, m1);
  auto m2 = find_vmethod_named(*cls, "addWorld");
  ASSERT_NE(nullptr, m2);
}
