/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "VerifyUtil.h"

/*
 * Check that testWithFinalField has WithFinalField's ctor inlined,
 * unfinalized its field and added write barrier.
 * testWithFinalFieldAndNoOptimize didn't inline WithFinalField's ctor because
 * of DoNotOptimize annotation And because the field is finalized, there are
 * write barrier added at the end of constructor.
 */

TEST_F(PreVerify, InlineWithFinalField) {
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto* m = find_vmethod_named(*cls, "testWithFinalField");
  ASSERT_NE(nullptr, m);
  auto* m_with_no_optimize =
      find_vmethod_named(*cls, "testWithFinalFieldAndNoOptimize");
  ASSERT_NE(nullptr, m_with_no_optimize);

  auto* final_field_cls =
      find_class_named(classes, "Lcom/facebook/redexinline/WithFinalField;");
  ASSERT_NE(nullptr, final_field_cls);
  auto* f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(nullptr,
            find_invoke(m,
                        DOPCODE_INVOKE_DIRECT_RANGE,
                        "<init>",
                        final_field_cls->get_type()));
  ASSERT_NE(nullptr,
            find_invoke(m_with_no_optimize,
                        DOPCODE_INVOKE_DIRECT_RANGE,
                        "<init>",
                        final_field_cls->get_type()));
}

TEST_F(PostVerify, InlineWithFinalField) {
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto* m = find_vmethod_named(*cls, "testWithFinalField");
  ASSERT_NE(nullptr, m);
  auto* m_with_no_optimize =
      find_vmethod_named(*cls, "testWithFinalFieldAndNoOptimize");
  ASSERT_NE(nullptr, m);

  auto* final_field_cls =
      find_class_named(classes, "Lcom/facebook/redexinline/WithFinalField;");
  ASSERT_NE(nullptr, final_field_cls);
  auto* f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_FALSE(is_final(f));
  auto* m_ctor = find_dmethod_named(*final_field_cls, "<init>");
  ASSERT_NE(nullptr, m_ctor);

  ASSERT_EQ(nullptr,
            find_invoke(m,
                        DOPCODE_INVOKE_DIRECT_RANGE,
                        "<init>",
                        final_field_cls->get_type()));
  auto testWithFinalField_str = stringify_for_comparision(m);
  auto expected = assembler::ircode_from_string(R"((
      (load-param-object v7)
      (new-instance "Lcom/facebook/redexinline/WithFinalField;")
      (move-result-pseudo-object v6)
      (const v5 3)
      (const v4 4)
      (const v3 5)
      (const v2 1)
      (const v1 2)
      (move-object v0 v6)
      (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
      (iput v3 v0 "Lcom/facebook/redexinline/WithFinalField;.finalField:I")
      (const v0 0)
      (sput v0 "Lredex/$StoreFenceHelper;.DUMMY_VOLATILE:I")
      (iget v6 "Lcom/facebook/redexinline/WithFinalField;.finalField:I")
      (move-result-pseudo v0)
      (invoke-static (v0) "Lorg/assertj/core/api/Assertions;.assertThat:(I)Lorg/assertj/core/api/AbstractIntegerAssert;")
      (move-result-object v0)
      (invoke-virtual (v0 v3) "Lorg/assertj/core/api/AbstractIntegerAssert;.isEqualTo:(I)Lorg/assertj/core/api/AbstractIntegerAssert;")
      (return-void)
  ))");
  EXPECT_EQ(testWithFinalField_str, assembler::to_string(expected.get()));

  // Because of DoNotOptimize, the constructor is not inlined in
  // testWithFinalFieldAndNoOptimize
  ASSERT_NE(nullptr,
            find_invoke(m_with_no_optimize,
                        DOPCODE_INVOKE_DIRECT_RANGE,
                        "<init>",
                        final_field_cls->get_type()));
  ASSERT_EQ(nullptr, find_instruction(m_with_no_optimize, DOPCODE_SPUT));

  // We also check that at the end of constructor there is write barrier added
  auto final_field_ctor_str = stringify_for_comparision(m_ctor);
  auto expected_ctor = assembler::ircode_from_string(R"((
      (load-param-object v1)
      (load-param v2)
      (load-param v3)
      (load-param v4)
      (load-param v5)
      (load-param v6)
      (invoke-direct (v1) "Ljava/lang/Object;.<init>:()V")
      (iput v2 v1 "Lcom/facebook/redexinline/WithFinalField;.finalField:I")
      (const v0 0)
      (sput v0 "Lredex/$StoreFenceHelper;.DUMMY_VOLATILE:I")
      (return-void)
  ))");
  EXPECT_EQ(final_field_ctor_str, assembler::to_string(expected_ctor.get()));
}

/*
 * Check that testWithFinalFieldAndFinalize don't inline
 * WithFinalFieldAndFinalize's ctor
 */

TEST_F(PreVerify, NoInlineWithFinalize) {
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto* m = find_vmethod_named(*cls, "testWithFinalFieldAndFinalize");
  ASSERT_NE(nullptr, m);

  auto* final_field_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/WithFinalFieldAndFinalize;");
  ASSERT_NE(nullptr, final_field_cls);
  auto* f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));
}

TEST_F(PostVerify, NoInlineWithFinalize) {
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto* m = find_vmethod_named(*cls, "testWithFinalFieldAndFinalize");
  ASSERT_NE(nullptr, m);

  auto* final_field_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/WithFinalFieldAndFinalize;");
  ASSERT_NE(nullptr, final_field_cls);
  auto* f = find_field_named(*final_field_cls, "finalField");
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
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto* m = find_vmethod_named(*cls, "testWithNormalField");
  ASSERT_NE(nullptr, m);

  auto* normal_field_cls =
      find_class_named(classes, "Lcom/facebook/redexinline/WithNormalField;");
  ASSERT_NE(nullptr, normal_field_cls);
  auto* f = find_field_named(*normal_field_cls, "normalField");
  ASSERT_NE(nullptr, f);
  ASSERT_FALSE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", normal_field_cls->get_type()));
}

TEST_F(PostVerify, InlineWithoutBarrier) {
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto* m = find_vmethod_named(*cls, "testWithNormalField");
  ASSERT_NE(nullptr, m);

  auto* normal_field_cls =
      find_class_named(classes, "Lcom/facebook/redexinline/WithNormalField;");
  ASSERT_NE(nullptr, normal_field_cls);
  auto* f = find_field_named(*normal_field_cls, "normalField");
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
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto* m = find_vmethod_named(*cls, "testWithFinalFieldTwoCtor");
  ASSERT_NE(nullptr, m);

  auto* final_field_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/WithFinalFieldTwoCtor;");
  ASSERT_NE(nullptr, final_field_cls);
  auto* f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));

  DexMethod* no_arg_ctor = nullptr;
  for (auto* ctor : final_field_cls->get_ctors()) {
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
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redexinline/MethodInlineRelaxedInitTest;");
  ASSERT_NE(nullptr, cls);
  auto* m = find_vmethod_named(*cls, "testWithFinalFieldTwoCtor");
  ASSERT_NE(nullptr, m);

  auto* final_field_cls = find_class_named(
      classes, "Lcom/facebook/redexinline/WithFinalFieldTwoCtor;");
  ASSERT_NE(nullptr, final_field_cls);
  auto* f = find_field_named(*final_field_cls, "finalField");
  ASSERT_NE(nullptr, f);
  ASSERT_TRUE(is_final(f));

  ASSERT_NE(
      nullptr,
      find_invoke(
          m, DOPCODE_INVOKE_DIRECT, "<init>", final_field_cls->get_type()));
  ASSERT_EQ(nullptr, find_instruction(m, DOPCODE_SPUT));

  DexMethod* no_arg_ctor = nullptr;
  for (auto* ctor : final_field_cls->get_ctors()) {
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
