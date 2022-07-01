/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>

#include "verify/VerifyUtil.h"

using namespace testing;

TEST_F(PreVerify, TestInputIsComplete) {
  auto root_cls =
      find_class_named(classes, "Lcom/facebook/redextest/RootInterface;");
  ASSERT_NE(nullptr, root_cls);
  auto super_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SuperInterface;");
  ASSERT_NE(nullptr, super_cls);

  auto a_cls = find_class_named(classes, "Lcom/facebook/redextest/AInterface;");
  ASSERT_NE(nullptr, a_cls);
  auto b_cls = find_class_named(classes, "Lcom/facebook/redextest/BInterface;");
  ASSERT_NE(nullptr, b_cls);
  auto ui_cls = find_class_named(
      classes, "Lcom/facebook/redextest/UnremovableInterface;");
  ASSERT_NE(nullptr, ui_cls);

  auto fa_cls =
      find_class_named(classes, "Lcom/facebook/redextest/FirstAModel;");
  ASSERT_NE(nullptr, fa_cls);
  auto sa_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SecondAModel;");
  ASSERT_NE(nullptr, sa_cls);

  auto fb_cls =
      find_class_named(classes, "Lcom/facebook/redextest/FirstBModel;");
  ASSERT_NE(nullptr, fb_cls);
  auto sb_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SecondBModel;");
  ASSERT_NE(nullptr, sb_cls);

  auto um_cls =
      find_class_named(classes, "Lcom/facebook/redextest/UnremovableModel;");
  ASSERT_NE(nullptr, um_cls);

  ASSERT_NE(nullptr, find_vmethod_named(*super_cls, "getInt"));
  ASSERT_NE(nullptr, find_vmethod_named(*super_cls, "getStr"));
  ASSERT_NE(nullptr, find_vmethod_named(*super_cls, "concat"));
  ASSERT_NE(nullptr, find_vmethod_named(*super_cls, "add"));

  ASSERT_NE(nullptr, find_vmethod_named(*a_cls, "getInt"));
  ASSERT_NE(nullptr, find_vmethod_named(*a_cls, "getStr"));
  ASSERT_NE(nullptr, find_vmethod_named(*a_cls, "concat"));
  ASSERT_NE(nullptr, find_vmethod_named(*a_cls, "add"));

  ASSERT_NE(nullptr, find_vmethod_named(*b_cls, "getInt"));
  ASSERT_NE(nullptr, find_vmethod_named(*b_cls, "getStr"));
  ASSERT_NE(nullptr, find_vmethod_named(*b_cls, "concat"));
  ASSERT_NE(nullptr, find_vmethod_named(*b_cls, "add"));

  ASSERT_NE(nullptr, find_vmethod_named(*fa_cls, "getInt"));
  ASSERT_NE(nullptr, find_vmethod_named(*fa_cls, "getStr"));
  ASSERT_NE(nullptr, find_vmethod_named(*fa_cls, "concat"));
  ASSERT_NE(nullptr, find_vmethod_named(*fa_cls, "add"));

  ASSERT_NE(nullptr, find_vmethod_named(*sa_cls, "getInt"));
  ASSERT_NE(nullptr, find_vmethod_named(*sa_cls, "getStr"));
  ASSERT_NE(nullptr, find_vmethod_named(*sa_cls, "concat"));
  ASSERT_NE(nullptr, find_vmethod_named(*sa_cls, "add"));

  ASSERT_NE(nullptr, find_vmethod_named(*fb_cls, "getInt"));
  ASSERT_NE(nullptr, find_vmethod_named(*fb_cls, "getStr"));
  ASSERT_NE(nullptr, find_vmethod_named(*fb_cls, "concat"));
  ASSERT_NE(nullptr, find_vmethod_named(*fb_cls, "add"));

  ASSERT_NE(nullptr, find_vmethod_named(*sb_cls, "getInt"));
  ASSERT_NE(nullptr, find_vmethod_named(*sb_cls, "getStr"));
  ASSERT_NE(nullptr, find_vmethod_named(*sb_cls, "concat"));
  ASSERT_NE(nullptr, find_vmethod_named(*sb_cls, "add"));

  EXPECT_THAT(fa_cls->get_interfaces()->get_type_list(),
              Contains(a_cls->get_type()));
  EXPECT_THAT(sa_cls->get_interfaces()->get_type_list(),
              Contains(a_cls->get_type()));
  EXPECT_THAT(fb_cls->get_interfaces()->get_type_list(),
              Contains(b_cls->get_type()));
  EXPECT_THAT(sb_cls->get_interfaces()->get_type_list(),
              Contains(b_cls->get_type()));

  EXPECT_THAT(um_cls->get_interfaces()->get_type_list(),
              Contains(ui_cls->get_type()));

  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/RemoveInterfaceTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testInvokeInterfaceSimple");
  ASSERT_NE(nullptr, m);

  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_INTERFACE, "getInt"));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_INTERFACE, "getStr"));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_INTERFACE, "concat"));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_INTERFACE, "add"));
}

TEST_F(PostVerify, InterfaceCallReplaced) {
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/RemoveInterfaceTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testInvokeInterfaceSimple");
  ASSERT_NE(nullptr, m);
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_INTERFACE, "getInt"));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_INTERFACE, "getStr"));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_INTERFACE, "concat"));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_INTERFACE, "add"));

  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_STATIC, "$dispatch$getInt"));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_STATIC, "$dispatch$getStr"));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_STATIC, "$dispatch$concat"));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_STATIC, "$dispatch$add"));
}

TEST_F(PostVerify, InterfaceInheritanceRemoved) {
  auto root_cls =
      find_class_named(classes, "Lcom/facebook/redextest/RootInterface;");
  ASSERT_NE(nullptr, root_cls);
  auto super_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SuperInterface;");
  ASSERT_NE(nullptr, super_cls);

  auto a_cls = find_class_named(classes, "Lcom/facebook/redextest/AInterface;");
  ASSERT_NE(nullptr, a_cls);
  auto b_cls = find_class_named(classes, "Lcom/facebook/redextest/BInterface;");
  ASSERT_NE(nullptr, b_cls);
  auto ui_cls = find_class_named(
      classes, "Lcom/facebook/redextest/UnremovableInterface;");
  ASSERT_NE(nullptr, ui_cls);

  auto fa_cls =
      find_class_named(classes, "Lcom/facebook/redextest/FirstAModel;");
  ASSERT_NE(nullptr, fa_cls);
  auto sa_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SecondAModel;");
  ASSERT_NE(nullptr, sa_cls);

  auto fb_cls =
      find_class_named(classes, "Lcom/facebook/redextest/FirstBModel;");
  ASSERT_NE(nullptr, fb_cls);
  auto sb_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SecondBModel;");
  ASSERT_NE(nullptr, sb_cls);

  auto um_cls =
      find_class_named(classes, "Lcom/facebook/redextest/UnremovableModel;");
  ASSERT_NE(nullptr, um_cls);

  EXPECT_THAT(fa_cls->get_interfaces()->get_type_list(),
              Not(Contains(a_cls->get_type())));
  EXPECT_THAT(sa_cls->get_interfaces()->get_type_list(),
              Not(Contains(a_cls->get_type())));
  EXPECT_THAT(fb_cls->get_interfaces()->get_type_list(),
              Not(Contains(b_cls->get_type())));
  EXPECT_THAT(sb_cls->get_interfaces()->get_type_list(),
              Not(Contains(b_cls->get_type())));

  EXPECT_THAT(fa_cls->get_interfaces()->get_type_list(),
              Contains(super_cls->get_type()));
  EXPECT_THAT(sa_cls->get_interfaces()->get_type_list(),
              Contains(super_cls->get_type()));
  EXPECT_THAT(fb_cls->get_interfaces()->get_type_list(),
              Contains(super_cls->get_type()));
  EXPECT_THAT(sb_cls->get_interfaces()->get_type_list(),
              Contains(super_cls->get_type()));

  EXPECT_THAT(um_cls->get_interfaces()->get_type_list(),
              Contains(ui_cls->get_type()));
}
