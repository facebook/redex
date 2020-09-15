/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "IRCode.h"
#include "Show.h"
#include "Trace.h"
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
    TRACE(INSTRUMENT, 1, "%s:%d", SHOW(method), code.sum_opcode_sizes());
  });
}

TEST_F(PostVerify, InstrumentVerify) {
  // Note: temporarily disabled for D8607279
  //
  // auto cls =
  //     find_class_named(classes, "Lcom/facebook/redextest/InstrumentTarget;");
  // ASSERT_NE(cls, nullptr);
  //
  // walk::methods(std::vector<DexClass*>{cls}, [](DexMethod* method) {
  //   const auto& full_name =
  //       method->get_class()->get_name()->str() + method->get_name()->str();
  //   // Only this one method should be instrumented.
  //   if (full_name == "Lcom/facebook/redextest/InstrumentTarget;func1") {
  //     EXPECT_NE(nullptr,
  //               find_invoke(method, DOPCODE_INVOKE_STATIC, "onMethodBegin"));
  //   } else {
  //     EXPECT_EQ(nullptr,
  //               find_invoke(method, DOPCODE_INVOKE_STATIC, "onMethodBegin"));
  //   }
  // });
  // cls = find_class_named(classes,
  //                        "Lcom/facebook/redextest/InstrumentTestClass1;");
  // ASSERT_NE(cls, nullptr);
  //
  // // This class is in blocklist. None of its methods should be instrumented.
  // walk::methods(std::vector<DexClass*>{cls}, [](DexMethod* method) {
  //   EXPECT_EQ(nullptr,
  //             find_invoke(method, DOPCODE_INVOKE_STATIC, "onMethodBegin"));
  // });
}
