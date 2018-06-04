/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>
#include <string>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "IRCode.h"
#include "VerifyUtil.h"
#include "Walkers.h"

TEST_F(PreVerify, InstrumentVerify) {
  ASSERT_NE(
      find_class_named(classes, "Lcom/facebook/redextest/InstrumentAnalysis;"),
      nullptr);
  auto cls =
      find_class_named(classes, "Lcom/facebook/redextest/InstrumentTarget;");
  ASSERT_NE(cls, nullptr);

  walk::methods(std::vector<DexClass*>{cls}, [](DexMethod* method) {
    EXPECT_EQ(nullptr,
              find_invoke(method, DOPCODE_INVOKE_STATIC, "onMethodBegin"));
  });

  walk::code(std::vector<DexClass*>{cls}, [](DexMethod* method, IRCode& code) {
    // There should be no instrumentation.
    TRACE(INSTRUMENT, 1, "%s:%d\n", SHOW(method), code.sum_opcode_sizes());
  });
}

TEST_F(PostVerify, InstrumentVerify) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redextest/InstrumentTarget;");
  ASSERT_NE(cls, nullptr);

  walk::methods(std::vector<DexClass*>{cls}, [](DexMethod* method) {
    EXPECT_NE(nullptr,
              find_invoke(method, DOPCODE_INVOKE_STATIC, "onMethodBegin"));
  });
}
