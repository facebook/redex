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

class RemoveNullcheckStringArgTest : public RedexTest {
 public:
  RemoveNullcheckStringArgTest() {
    create_class(DexType::make_type("Lkotlin/jvm/internal/Intrinsics;"),
                 type::java_lang_Object(), {}, ACC_PUBLIC);
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
  actual_code->build_cfg();
  RemoveNullcheckStringArg pass;
  RemoveNullcheckStringArg::TransferMap transferMap;
  RemoveNullcheckStringArg::NewMethodSet newMethods;
  pass.setup(transferMap, newMethods);
  pass.change_in_cfg(actual_code->cfg(), transferMap);
  actual_code->clear_cfg();

  auto expected_str = R"(
    (
     (load-param-object v0)
     (const-string "args")
     (move-result-pseudo-object v1)
     (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter:(Ljava/lang/Object;)V")
     (invoke-static (v0) "Lkotlin/jvm/internal/Intrinsics;.$WrCheckExpression:(Ljava/lang/Object;)V")
     (return-void)
    )
  )";

  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), actual_code.get());
}
