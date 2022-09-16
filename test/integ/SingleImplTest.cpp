/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "RedexTest.h"
#include "SingleImpl.h"

class SingleImplTest : public RedexIntegrationTest {
 public:
  void SetUp() override {
    DexType* m_helper = DexType::get_type("Lcom/facebook/redextest/Helper;");

    auto create_intf = assembler::method_from_string(R"(
      (method (public static) "Lcom/facebook/redextest/Helper;.createIntf:()Ljava/lang/Object;"
       (
        (new-instance "Lcom/facebook/redextest/Impl;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Lcom/facebook/redextest/Impl;.<init>:()V")
        (return-object v0)
       )
      )
    )");

    auto ret_intf = assembler::method_from_string(R"(
      (method (public static) "Lcom/facebook/redextest/Helper;.retIntf:()Lcom/facebook/redextest/Intf;"
       (
        (invoke-static () "Lcom/facebook/redextest/Helper;.createIntf:()Ljava/lang/Object;")
        (move-result-object v0)
        (return-object v0)
       )
      )
    )");

    type_class(m_helper)->add_method(create_intf);
    type_class(m_helper)->add_method(ret_intf);
  }
};

/**
 * If we replace an interface declared as the return type of a method with a
 * class, we want to make sure the return-object does not introduce a type
 * error. If the return-object relies on the more relaxed to-interface type
 * checking, we make sure a desired check-cast is inserted as needed.
 */
TEST_F(SingleImplTest, RemoveReturnTypeInterfaceTest) {
  auto scope = build_class_scope(stores);

  auto si = std::make_unique<SingleImplPass>();
  std::vector<Pass*> passes{si.get()};
  run_passes(passes);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (invoke-static () "Lcom/facebook/redextest/Helper;.createIntf:()Ljava/lang/Object;")
     (move-result-object v0)
     (check-cast v0 "Lcom/facebook/redextest/Impl;")
     (move-result-pseudo-object v1)
     (return-object v1)
    )
  )");

  auto ret_impl = DexMethod::get_method(
      "Lcom/facebook/redextest/Helper;.retIntf:()Lcom/"
      "facebook/redextest/Impl;");
  EXPECT_CODE_EQ(ret_impl->as_def()->get_code(), expected_code.get());
}
