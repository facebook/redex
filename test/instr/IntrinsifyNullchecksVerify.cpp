/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Resolver.h"
#include "Show.h"
#include "verify/VerifyUtil.h"

namespace {
constexpr const char* class_null_check = "Lredex/$NullCheck;";
constexpr const char* class_test_obj = "Lredex/NullCheckConversionTest;";
} // namespace

TEST_F(PreVerify, TestNullCheck) {
  auto* test_obj_cls = find_class_named(classes, class_test_obj);
  EXPECT_NE(nullptr, test_obj_cls);

  auto* meth_init = find_dmethod_named(*test_obj_cls, "<init>");
  ASSERT_NE(nullptr, meth_init);
  // Before opt, there is a invoke-virtual Object;.getClass();
  EXPECT_NE(nullptr,
            find_invoke(meth_init, DOPCODE_INVOKE_VIRTUAL, "getClass"));
}

TEST_F(PostVerify, TestNullCheck) {
  auto* test_obj_cls = find_class_named(classes, class_test_obj);
  EXPECT_NE(nullptr, test_obj_cls);

  auto* null_check_cls = find_class_named(classes, class_null_check);
  EXPECT_NE(nullptr, null_check_cls);

  auto* meth_init = find_dmethod_named(*test_obj_cls, "<init>");
  ASSERT_NE(nullptr, meth_init);
  // After opt, getClass() should be replaced with a null_check.
  EXPECT_EQ(nullptr,
            find_invoke(meth_init, DOPCODE_INVOKE_VIRTUAL, "getClass"));
  EXPECT_NE(nullptr,
            find_invoke(meth_init, DOPCODE_INVOKE_STATIC, "null_check"));
}
