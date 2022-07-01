/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "DexInstruction.h"
#include "VerifyUtil.h"

TEST_F(PreVerify, NoTypeChecks) {
  auto assert_handler_cls = find_class_named(
      classes, "Lcom/facebook/redex/ConstantPropagationAssertHandler;");
  ASSERT_NE(nullptr, assert_handler_cls);
  auto field_error_m =
      find_dmethod_named(*assert_handler_cls, "fieldValueError");
  ASSERT_NE(nullptr, field_error_m);
  auto method_error_m =
      find_dmethod_named(*assert_handler_cls, "returnValueError");
  ASSERT_NE(nullptr, method_error_m);

  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/TypeAnalysisAssertsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto test_field_m = find_vmethod_named(*test_cls, "getBase");
  auto test_return_m = find_vmethod_named(*test_cls, "testSetAndGet");
  ASSERT_NE(nullptr, test_field_m);
  ASSERT_NE(nullptr, test_return_m);
  ASSERT_EQ(nullptr,
            find_invoke(test_field_m, DOPCODE_INVOKE_STATIC, "fieldValueError",
                        nullptr));
  ASSERT_EQ(nullptr,
            find_invoke(test_return_m, DOPCODE_INVOKE_STATIC,
                        "returnValueError", nullptr));
}

TEST_F(PostVerify, HasTypeChecks) {
  auto assert_handler_cls = find_class_named(
      classes, "Lcom/facebook/redex/ConstantPropagationAssertHandler;");
  ASSERT_NE(nullptr, assert_handler_cls);
  auto field_error_m =
      find_dmethod_named(*assert_handler_cls, "fieldValueError");
  ASSERT_NE(nullptr, field_error_m);
  auto method_error_m =
      find_dmethod_named(*assert_handler_cls, "returnValueError");
  ASSERT_NE(nullptr, method_error_m);

  auto test_cls = find_class_named(
      classes, "Lcom/facebook/redextest/TypeAnalysisAssertsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto test_field_m = find_vmethod_named(*test_cls, "getBase");
  auto test_return_m = find_vmethod_named(*test_cls, "testSetAndGet");
  ASSERT_NE(nullptr, test_field_m);
  ASSERT_NE(nullptr, test_return_m);
  ASSERT_NE(nullptr,
            find_invoke(test_field_m, DOPCODE_INVOKE_STATIC, "fieldValueError",
                        nullptr));
  ASSERT_NE(nullptr,
            find_invoke(test_return_m, DOPCODE_INVOKE_STATIC,
                        "returnValueError", nullptr));
}
