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

/*
 * Check that testWithFinalField didn't inline WithFinalField's ctor.
 */

TEST_F(PreVerify, InlineWithFinalField) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "testWithFinalField");
  ASSERT_NE(nullptr, m);

  auto final_field_cls =
      find_class_named(classes, "Lcom/facebook/redexinline/WithFinalField;");
  ASSERT_NE(nullptr, final_field_cls);
  auto f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));
}

TEST_F(PostVerify, InlineWithFinalField) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "testWithFinalField");
  ASSERT_NE(nullptr, m);

  auto final_field_cls =
      find_class_named(classes, "Lcom/facebook/redexinline/WithFinalField;");
  ASSERT_NE(nullptr, final_field_cls);
  auto f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));
  ASSERT_EQ(nullptr, find_instruction(m, DOPCODE_SPUT));
}

/*
 * Check that testWithFinalFieldAndFinalize don't inline
 * WithFinalFieldAndFinalize's ctor
 */

TEST_F(PreVerify, NoInlineWithFinalize) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "testWithFinalFieldAndFinalize");
  ASSERT_NE(nullptr, m);

  auto final_field_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/WithFinalFieldAndFinalize;");
  ASSERT_NE(nullptr, final_field_cls);
  auto f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));
}

TEST_F(PostVerify, NoInlineWithFinalize) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "testWithFinalFieldAndFinalize");
  ASSERT_NE(nullptr, m);

  auto final_field_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/WithFinalFieldAndFinalize;");
  ASSERT_NE(nullptr, final_field_cls);
  auto f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));
  ASSERT_EQ(nullptr, find_instruction(m, DOPCODE_SPUT));
}

/*
 * Check that testWithNormalField inline WithNormalField's ctor
 * but don't add write barrier
 */

TEST_F(PreVerify, InlineWithoutBarrier) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "testWithNormalField");
  ASSERT_NE(nullptr, m);

  auto normal_field_cls =
      find_class_named(classes, "Lcom/facebook/redexinline/WithNormalField;");
  ASSERT_NE(nullptr, normal_field_cls);
  auto f = find_field_named(*normal_field_cls, "normalField");
  ASSERT_NE(nullptr, f);
  ASSERT_FALSE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", normal_field_cls->get_type()));
}

TEST_F(PostVerify, InlineWithoutBarrier) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "testWithNormalField");
  ASSERT_NE(nullptr, m);

  auto normal_field_cls =
      find_class_named(classes, "Lcom/facebook/redexinline/WithNormalField;");
  ASSERT_NE(nullptr, normal_field_cls);
  auto f = find_field_named(*normal_field_cls, "normalField");
  ASSERT_NE(nullptr, f);
  ASSERT_FALSE(is_final(f));

  ASSERT_EQ(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", normal_field_cls->get_type()));
  ASSERT_EQ(nullptr, find_instruction(m, DOPCODE_SPUT));
}

/*
 * Check that WithFinalFieldTwoCtor's no-arg ctor inlined one-arg ctor
 * WithNormalField's ctor field are not finalized and no write barrier added
 * no-arg ctor are not inlined into testWithFinalFieldTwoCtor.
 */

TEST_F(PreVerify, InlineTwoCtorClass) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "testWithFinalFieldTwoCtor");
  ASSERT_NE(nullptr, m);

  auto final_field_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/WithFinalFieldTwoCtor;");
  ASSERT_NE(nullptr, final_field_cls);
  auto f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));

  DexMethod* no_arg_ctor = nullptr;
  for (auto ctor : final_field_cls->get_ctors()) {
    if (ctor->get_proto()->get_args()->empty()) {
      no_arg_ctor = ctor;
      break;
    }
  }
  ASSERT_NE(nullptr, no_arg_ctor);
  ASSERT_NE(nullptr,
            find_invoke(no_arg_ctor,
                        DOPCODE_INVOKE_DIRECT,
                        "<init>",
                        final_field_cls->get_type()));
}

TEST_F(PostVerify, InlineTwoCtorClass) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto m = find_vmethod_named(*cls, "testWithFinalFieldTwoCtor");
  ASSERT_NE(nullptr, m);

  auto final_field_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/WithFinalFieldTwoCtor;");
  ASSERT_NE(nullptr, final_field_cls);
  auto f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));
  ASSERT_EQ(nullptr, find_instruction(m, DOPCODE_SPUT));

  DexMethod* no_arg_ctor = nullptr;
  for (auto ctor : final_field_cls->get_ctors()) {
    if (ctor->get_proto()->get_args()->empty()) {
      no_arg_ctor = ctor;
      break;
    }
  }
  ASSERT_NE(nullptr, no_arg_ctor);
  ASSERT_EQ(nullptr,
            find_invoke(no_arg_ctor,
                        DOPCODE_INVOKE_DIRECT,
                        "<init>",
                        final_field_cls->get_type()));
}
