/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "VerifyUtil.h"

/*
 * Ensure that the instrumentation test in InlineTest.java is actually testing
 * inlined code by checking that the invoke-direct/range opcode is removed
 * in the redexed binary.
 */

TEST_F(PreVerify, InlineInvokeRange) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  ASSERT_NE(nullptr, cls);

  auto m = find_vmethod_named(*cls, "testInvokeRange");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_instruction(m, OPCODE_INVOKE_DIRECT_RANGE));
}

TEST_F(PostVerify, InlineInvokeRange) {
  auto cls = find_class_named(
    classes, "Lcom/facebook/redexinline/InlineTest;");
  ASSERT_NE(nullptr, cls);

  auto m = find_vmethod_named(*cls, "testInvokeRange");
  ASSERT_NE(nullptr, m);
  ASSERT_EQ(nullptr, find_instruction(m, OPCODE_INVOKE_DIRECT_RANGE));
}
