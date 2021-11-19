/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Resolver.h"
#include "verify/VerifyUtil.h"

namespace {
constexpr const char* class_name = "LKtNonCapturingLambda;";
constexpr const char* fn2 = "Lkotlin/jvm/functions/Function2;";
constexpr const char* foo = "LKtNonCapturingLambda$foo$1;";
constexpr const char* foo1 = "LKtNonCapturingLambda$foo1$1;";
} // namespace

TEST_F(PreVerify, KotlinGeneratedClass) {
  auto* cls = find_class_named(classes, class_name);
  EXPECT_NE(nullptr, cls);
  auto* intf_cls = find_class_named(classes, fn2);
  EXPECT_NE(nullptr, intf_cls);

  auto* meth_doCalc = find_vmethod_named(*cls, "doCalc");
  EXPECT_NE(nullptr, meth_doCalc);
  // Before opt, there is invoke-interface call, but no invoke-virtual call in
  // doCalc.
  ASSERT_NE(nullptr, find_invoke(meth_doCalc, DOPCODE_INVOKE_INTERFACE,
                                 "invoke", intf_cls->get_type()));
  ASSERT_EQ(nullptr,
            find_invoke(meth_doCalc, DOPCODE_INVOKE_VIRTUAL, "invoke"));

  auto* meth_doCalc1 = find_vmethod_named(*cls, "doCalc1");
  EXPECT_NE(nullptr, meth_doCalc1);
  // Before opt, there is invoke-interface call, but no invoke-virtual call in
  // doCalc1.
  ASSERT_NE(nullptr, find_invoke(meth_doCalc1, DOPCODE_INVOKE_INTERFACE,
                                 "invoke", intf_cls->get_type()));
  ASSERT_EQ(nullptr,
            find_invoke(meth_doCalc1, DOPCODE_INVOKE_VIRTUAL, "invoke"));
}

TEST_F(PostVerify, KotlinGeneratedClass) {
  auto* cls = find_class_named(classes, class_name);
  EXPECT_NE(nullptr, cls);

  auto* meth_doCalc = find_vmethod_named(*cls, "doCalc");
  EXPECT_NE(nullptr, meth_doCalc);

  // After opt, there is no invoke-interface, which is replaced with
  // invoke-virtual in doCalc.
  auto* intf_cls = find_class_named(classes, fn2);
  ASSERT_EQ(nullptr, find_invoke(meth_doCalc, DOPCODE_INVOKE_INTERFACE,
                                 "invoke", intf_cls->get_type()));
  auto* impl_cls = find_class_named(classes, foo);
  ASSERT_NE(nullptr, find_invoke(meth_doCalc, DOPCODE_INVOKE_VIRTUAL, "invoke",
                                 impl_cls->get_type()));

  auto* meth_doCalc1 = find_vmethod_named(*cls, "doCalc1");
  EXPECT_NE(nullptr, meth_doCalc1);
  // After opt, there is no invoke-interface, which is replaced with
  // invoke-virtual in doCalc1.
  ASSERT_EQ(nullptr, find_invoke(meth_doCalc1, DOPCODE_INVOKE_INTERFACE,
                                 "invoke", intf_cls->get_type()));
  impl_cls = find_class_named(classes, foo1);
  ASSERT_NE(nullptr, find_invoke(meth_doCalc1, DOPCODE_INVOKE_VIRTUAL, "invoke",
                                 impl_cls->get_type()));
}
