/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "PureMethods.h"
#include "RedexTest.h"

class PureMethodTest : public RedexIntegrationTest {
 protected:
  DexMethod* get_method(const std::string& name) {
    return DexMethod::get_method(name)->as_def();
  }
};

TEST_F(PureMethodTest, VirtuamMethodTest) {
  auto scope = build_class_scope(stores);
  AnalyzePureMethodsPass pass;
  pass.analyze_and_set_pure_methods(scope);
  auto b_f0 = get_method("Lcom/facebook/redextest/Base;.fn0:()I");
  auto one_f0 = get_method("Lcom/facebook/redextest/SubOne;.fn0:()I");
  auto two_f0 = get_method("Lcom/facebook/redextest/SubOne;.fn0:()I");

  EXPECT_TRUE(b_f0->rstate.pure_method() == true);
  EXPECT_TRUE(one_f0->rstate.pure_method() == true);
  EXPECT_TRUE(two_f0->rstate.pure_method() == true);
  auto b_f3 = get_method(
      "Lcom/facebook/redextest/Base;.fn3:(Ljava/lang/String;)Ljava/lang/"
      "String;");
  EXPECT_TRUE(b_f3->rstate.pure_method() == false);
  auto b_f4 = get_method("Lcom/facebook/redextest/Base;.fn4:(II)I");
  EXPECT_TRUE(b_f4->rstate.pure_method() == false);
  auto b_f5 = get_method(
      "Lcom/facebook/redextest/Base;.fn5:(Ljava/lang/String;I)Ljava/lang/"
      "String;");
  EXPECT_TRUE(b_f5->rstate.pure_method() == false);
  auto b_f6 = get_method(
      "Lcom/facebook/redextest/Base;.fn6:(Ljava/lang/String;)Ljava/lang/"
      "String;");
  EXPECT_TRUE(b_f6->rstate.pure_method() == false);
}
