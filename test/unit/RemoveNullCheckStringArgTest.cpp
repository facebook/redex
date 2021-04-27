/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "RedexTest.h"
#include "RemoveNullcheckStringArg.h"
#include "ScopeHelper.h"
#include "ScopedCFG.h"

class RemoveNullcheckStringArgTest : public RedexTest {
 public:
  RemoveNullcheckStringArgTest() {
    create_class(DexType::make_type("Lkotlin/jvm/internal/Intrinsics;"),
                 type::java_lang_Object(), {}, ACC_PUBLIC);
    create_class(DexType::make_type("Ljava/lang/StringBuilder;"),
                 type::java_lang_Object(), {}, ACC_PUBLIC);

    DexMethod::make_method("Ljava/lang/Integer;.toString:(I)Ljava/lang/String;")
        ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
    ;
    DexMethod::make_method("Ljava/lang/StringBuilder;.<init>:()V")
        ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
    ;
    DexMethod::make_method(
        "Ljava/lang/StringBuilder;.append:(Ljava/lang/"
        "String;)Ljava/lang/StringBuilder;")
        ->make_concrete(ACC_PUBLIC, false /* is_virtual */);
    ;
    DexMethod::make_method(
        "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        ->make_concrete(ACC_PUBLIC, false /* is_virtual */);
    ;
  }
};

TEST_F(RemoveNullcheckStringArgTest, simple) {
  auto str = R"(
    (
     (load-param-object v0)
     (const-string "args")
     (move-result-pseudo-object v1)
     (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
     (invoke-static (v0 v1) "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
     (return-void)
    )
  )";

  auto actual_code = assembler::ircode_from_string(str);
  {
    cfg::ScopedCFG cfg(actual_code.get());
    RemoveNullcheckStringArg pass;
    RemoveNullcheckStringArg::TransferMap transferMap;
    RemoveNullcheckStringArg::NewMethodSet newMethods;
    pass.setup(transferMap, newMethods);
    pass.change_in_cfg(*cfg, transferMap, false);
  }

  auto expected_str = R"(
    (
     (load-param-object v0)
     (const-string "args")
     (move-result-pseudo-object v1)
     (const v2 0)
     (invoke-static (v0 v2) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_3:(Ljava/lang/Object;I)V")
     (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression_V1_3:(Ljava/lang/Object;)V")
     (return-void)
    )
  )";

  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), actual_code.get());
}

TEST_F(RemoveNullcheckStringArgTest, simpleVirtual) {
  auto str = R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (const-string "args")
     (move-result-pseudo-object v2)
     (invoke-static (v1 v2) "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
     (invoke-static (v1 v2) "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
     (return-void)
    )
  )";

  auto actual_code = assembler::ircode_from_string(str);
  {
    cfg::ScopedCFG cfg(actual_code.get());
    RemoveNullcheckStringArg pass;
    RemoveNullcheckStringArg::TransferMap transferMap;
    RemoveNullcheckStringArg::NewMethodSet newMethods;
    pass.setup(transferMap, newMethods);
    pass.change_in_cfg(*cfg, transferMap, true);
  }

  auto expected_str = R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (const-string "args")
     (move-result-pseudo-object v2)
     (const v3 0)
     (invoke-static (v1 v3) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_3:(Ljava/lang/Object;I)V")
     (invoke-static (v1) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression_V1_3:(Ljava/lang/Object;)V")
     (return-void)
    )
  )";

  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), actual_code.get());
}

TEST_F(RemoveNullcheckStringArgTest, simpleiVirtualiCpy) {
  auto str = R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (const-string "args")
     (move-result-pseudo-object v2)
     (move v3 v1)
     (invoke-static (v3 v2) "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
     (invoke-static (v3 v2) "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
     (return-void)
    )
  )";

  auto actual_code = assembler::ircode_from_string(str);
  {
    cfg::ScopedCFG cfg(actual_code.get());
    RemoveNullcheckStringArg pass;
    RemoveNullcheckStringArg::TransferMap transferMap;
    RemoveNullcheckStringArg::NewMethodSet newMethods;
    pass.setup(transferMap, newMethods);
    pass.change_in_cfg(*cfg, transferMap, true);
  }

  auto expected_str = R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (const-string "args")
     (move-result-pseudo-object v2)
     (move v3 v1)
     (const v4 0)
     (invoke-static (v3 v4) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_3:(Ljava/lang/Object;I)V")
     (invoke-static (v3) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression_V1_3:(Ljava/lang/Object;)V")
     (return-void)
    )
  )";

  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), actual_code.get());
}

TEST_F(RemoveNullcheckStringArgTest, simpleStatic) {
  auto str = R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (const-string "args")
     (move-result-pseudo-object v2)
     (invoke-static (v1 v2) "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
     (invoke-static (v1 v2) "Lkotlin/jvm/internal/Intrinsics;.checkExpressionValueIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
     (return-void)
    )
  )";

  auto actual_code = assembler::ircode_from_string(str);
  {
    cfg::ScopedCFG cfg(actual_code.get());
    RemoveNullcheckStringArg pass;
    RemoveNullcheckStringArg::TransferMap transferMap;
    RemoveNullcheckStringArg::NewMethodSet newMethods;
    pass.setup(transferMap, newMethods);
    pass.change_in_cfg(*cfg, transferMap, false);
  }

  auto expected_str = R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (const-string "args")
     (move-result-pseudo-object v2)
     (const v3 1)
     (invoke-static (v1 v3) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter_V1_3:(Ljava/lang/Object;I)V")
     (invoke-static (v1) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression_V1_3:(Ljava/lang/Object;)V")
     (return-void)
    )
  )";

  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), actual_code.get());
}
