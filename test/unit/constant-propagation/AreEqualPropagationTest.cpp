/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace std::string_view_literals;

#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

struct AreEqualPropagationTest : public ConstantPropagationTest {};

namespace {

// When second arg is non-null and both args are safe symmetric-equals types,
// swap args so the non-null one becomes first.
struct TypePairParam {
  const char* name;
  std::string_view type0;
  std::string_view type1;
};

class AreEqualSwapSafeTypesTest
    : public AreEqualPropagationTest,
      public ::testing::WithParamInterface<TypePairParam> {};

TEST_P(AreEqualSwapSafeTypesTest, SecondArgNonNullBothSafeTypes_ArgsSwapped) {
  const auto& param = GetParam();
  std::string method_str = R"(
    (method (static) "LFoo;.test:($T0$T1)Z"
     (
      (load-param-object v0)
      (load-param-object v1)
      (if-eqz v1 :is_null)
      (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z")
      (move-result v2)
      (return v2)
      (:is_null)
      (const v2 0)
      (return v2)
     )
    )
  )";
  method_str.replace(method_str.find("$T0"), 3, param.type0);
  method_str.replace(method_str.find("$T1"), 3, param.type1);
  auto* m = assembler::method_from_string(method_str);
  do_const_prop(m);

  std::string expected_str = R"(
    (
      (load-param-object v0)
      (load-param-object v1)
      (if-eqz v1 :is_null)
      (invoke-static (v1 v0) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z")
      (move-result v2)
      (return v2)
      (:is_null)
      (const v2 0)
      (return v2)
    )
  )";
  const auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(m->get_code(), expected_code.get());
}

INSTANTIATE_TEST_SUITE_P(
    AreEqual,
    AreEqualSwapSafeTypesTest,
    ::testing::Values(TypePairParam{"Same_Safe_Type", "Ljava/lang/String;",
                                    "Ljava/lang/String;"},
                      TypePairParam{"Different_Safe_Types",
                                    "Ljava/lang/String;",
                                    "Ljava/lang/Integer;"}),
    [](const auto& info) { return info.param.name; });

// When at least one arg is not a safe type, the swap must not fire.

class AreEqualNoSwapUnsafeTypeTest
    : public AreEqualPropagationTest,
      public ::testing::WithParamInterface<TypePairParam> {};

TEST_P(AreEqualNoSwapUnsafeTypeTest, SecondArgNonNullUnsafeType_Retained) {
  const auto& param = GetParam();
  std::string method_str = R"(
    (method (static) "LFoo;.test:($T0$T1)Z"
     (
      (load-param-object v0)
      (load-param-object v1)
      (if-eqz v1 :is_null)
      (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z")
      (move-result v2)
      (return v2)
      (:is_null)
      (const v2 0)
      (return v2)
     )
    )
  )";
  method_str.replace(method_str.find("$T0"), 3, param.type0);
  method_str.replace(method_str.find("$T1"), 3, param.type1);
  auto* m = assembler::method_from_string(method_str);
  const auto expected = assembler::to_s_expr(m->get_code());
  do_const_prop(m);
  EXPECT_EQ(assembler::to_s_expr(m->get_code()), expected);
}

INSTANTIATE_TEST_SUITE_P(
    AreEqualNoSwapUnsafeTypeTests,
    AreEqualNoSwapUnsafeTypeTest,
    ::testing::Values(TypePairParam{"Neither_Safe", "Ljava/lang/Object;",
                                    "Ljava/lang/Object;"},
                      TypePairParam{"First_Safe_Second_Unsafe",
                                    "Ljava/lang/String;", "Ljava/lang/Object;"},
                      TypePairParam{"First_Unsafe_Second_Safe",
                                    "Ljava/lang/Object;", "Ljava/lang/String;"},
                      TypePairParam{"Both_Concrete_Unsafe", "Ljava/util/List;",
                                    "Ljava/util/List;"}),
    [](const auto& info) { return info.param.name; });

// When the first arg is known null, areEqual must be retained (cannot call
// equals on null).
TEST_F(AreEqualPropagationTest, AreEqualFirstArgNull_Retained) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (load-param-object v1)
      (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z")
      (move-result v2)
      (return v2)
    )
  )");
  const auto expected = assembler::to_s_expr(code.get());
  do_const_prop(code.get());
  EXPECT_EQ(assembler::to_s_expr(code.get()), expected);
}

// When both args are known non-null, swapping gains nothing.
TEST_F(AreEqualPropagationTest, BothArgsNonNull_Retained) {
  auto* m = assembler::method_from_string(R"(
    (method (static) "LFoo;.test:(Ljava/lang/String;Ljava/lang/String;)Z"
     (
      (load-param-object v0)
      (load-param-object v1)
      (if-eqz v0 :is_null)
      (if-eqz v1 :is_null)
      (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z")
      (move-result v2)
      (return v2)
      (:is_null)
      (const v2 0)
      (return v2)
     )
    )
  )");
  const auto expected = assembler::to_s_expr(m->get_code());
  do_const_prop(m);
  EXPECT_EQ(assembler::to_s_expr(m->get_code()), expected);
}

// When the first arg is known null, swapping gains nothing and can hurt:
// it routes through b.equals(null), which is opaque to CP.
TEST_F(AreEqualPropagationTest, FirstArgKnownNull_Retained) {
  auto* m = assembler::method_from_string(R"(
    (method (static) "LFoo;.test:(Ljava/lang/String;Ljava/lang/String;)Z"
     (
      (load-param-object v0)
      (load-param-object v1)
      (if-nez v0 :first_nonnull)
      (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z")
      (move-result v2)
      (return v2)
      (:first_nonnull)
      (const v2 1)
      (return v2)
     )
    )
  )");
  const auto expected = assembler::to_s_expr(m->get_code());
  do_const_prop(m);
  EXPECT_EQ(assembler::to_s_expr(m->get_code()), expected);
}

// --- Verification tests ---
// These test verify_areequal_semantics(), which is called once in main.cpp
// before any parallel walk to confirm that Intrinsics.areEqual has the
// expected semantics.

// Base for verification tests. Registers Object.equals.
struct AreEqualVerificationTest : public ConstantPropagationTest {
  AreEqualVerificationTest() {
    DexMethod::make_method("Ljava/lang/Object;.equals:(Ljava/lang/Object;)Z");
  }
};

// Construct Intrinsics.areEqual with its real implementation and verify
// that verify_areequal_semantics accepts it.
TEST_F(AreEqualVerificationTest, CorrectImplementation_VerificationPasses) {
  assembler::method_from_string(R"(
    (method (public static) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z"
     (
      (load-param-object v0)
      (load-param-object v1)
      (if-nez v0 :non_null)
      (if-nez v1 :first_null_second_nonnull)
      (const v0 1)
      (goto :return)
      (:first_null_second_nonnull)
      (const v0 0)
      (goto :return)
      (:non_null)
      (invoke-virtual (v0 v1) "Ljava/lang/Object;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (:return)
      (return v0)
     )
    )
  )");
  EXPECT_EQ(cp::verify_areequal_semantics(), std::nullopt);
}

struct InvokeParam {
  std::string name;
  std::string_view invoke_insns;
  std::string_view expected_substr;
};

class InvokeVerificationTest
    : public AreEqualVerificationTest,
      public ::testing::WithParamInterface<InvokeParam> {};

TEST_P(InvokeVerificationTest, Detected) {
  std::string body = R"(
    (method (public static) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z"
     (
      (load-param-object v0)
      (load-param-object v1)
      (if-nez v0 :non_null)
      (if-nez v1 :first_null_second_nonnull)
      (const v0 1)
      (return v0)
      (:first_null_second_nonnull)
      (const v0 0)
      (return v0)
      (:non_null)
      $INVOKE_INSNS
      (return v0)
     )
    )
  )";
  body.replace(body.find("$INVOKE_INSNS"), 13, GetParam().invoke_insns);
  assembler::method_from_string(body);
  auto err = cp::verify_areequal_semantics();
  ASSERT_TRUE(err.has_value());
  EXPECT_THAT(*err, ::testing::HasSubstr(GetParam().expected_substr));
}

INSTANTIATE_TEST_SUITE_P(
    InvokeVerificationTests,
    InvokeVerificationTest,
    ::testing::Values(
        InvokeParam{"NoInvoke", R"((const v0 0))", "0 Object.equals calls"},
        InvokeParam{
            "WrongMethod",
            R"((invoke-virtual (v0 v1) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
               (move-result v0))",
            "0 Object.equals calls"},
        InvokeParam{"ExtraInvoke",
                    R"((invoke-virtual (v0) "Ljava/lang/Object;.hashCode:()I")
               (invoke-virtual (v0 v1) "Ljava/lang/Object;.equals:(Ljava/lang/Object;)Z")
               (move-result v0))",
                    "2 invokes"}),
    [](const auto& info) { return info.param.name; });

struct BranchParam {
  std::string name;
  std::string_view if_block;
  std::string_view expected_substr;
};

class BranchVerificationTest
    : public AreEqualVerificationTest,
      public ::testing::WithParamInterface<BranchParam> {};

TEST_P(BranchVerificationTest, Detected) {
  std::string body = R"(
    (method (public static) "Lkotlin/jvm/internal/Intrinsics;.areEqual:(Ljava/lang/Object;Ljava/lang/Object;)Z"
     (
      (load-param-object v0)
      (load-param-object v1)
      $IF_BLOCK
      (:past_checks)
      (invoke-virtual (v0 v1) "Ljava/lang/Object;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (return v0)
     )
    )
  )";
  body.replace(body.find("$IF_BLOCK"), 9, GetParam().if_block);
  assembler::method_from_string(body);
  auto err = cp::verify_areequal_semantics();
  ASSERT_TRUE(err.has_value());
  EXPECT_THAT(*err, ::testing::HasSubstr(GetParam().expected_substr));
}

INSTANTIATE_TEST_SUITE_P(
    BranchVerificationTests,
    BranchVerificationTest,
    ::testing::Values(BranchParam{"ZeroNullChecks", "", "0 null checks"},
                      BranchParam{"OneNullCheck", "(if-nez v0 :past_checks)",
                                  "1 null checks"},
                      BranchParam{"ThreeNullChecks",
                                  R"((if-nez v0 :past_checks)
                            (if-nez v0 :past_checks)
                            (if-nez v0 :past_checks))",
                                  "3 null checks"},
                      BranchParam{"ExtraNonNullCheckBranch",
                                  R"((if-nez v0 :past_checks)
                            (if-nez v0 :past_checks)
                            (if-eq v0 v1 :past_checks))",
                                  "3 branches"},
                      BranchParam{"OneNullCheckOneOtherBranch",
                                  R"((if-nez v0 :past_checks)
                            (if-eq v0 v1 :past_checks))",
                                  "1 null checks"}),
    [](const auto& info) { return info.param.name; });

} // namespace
