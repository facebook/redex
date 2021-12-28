/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "DexInstruction.h"
#include "VerifyUtil.h"

TEST_F(PreVerify, SimpleInvokeVirtual) {
  auto base_cls = find_class_named(classes, "Lcom/facebook/redextest/Base;");
  ASSERT_NE(nullptr, base_cls);
  auto sub1_cls = find_class_named(classes, "Lcom/facebook/redextest/SubOne;");
  ASSERT_NE(nullptr, sub1_cls);
  auto sub2_cls = find_class_named(classes, "Lcom/facebook/redextest/SubTwo;");
  ASSERT_NE(nullptr, sub2_cls);
  auto sub3_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SubThree;");
  ASSERT_NE(nullptr, sub3_cls);

  // Verify invoke-virtual bindings.
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/ResolveRefsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testSimpleInvokeVirtual");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 base_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub1_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub2_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub3_cls->get_type()));
}

TEST_F(PostVerify, SimpleInvokeVirtual) {
  auto base_cls = find_class_named(classes, "Lcom/facebook/redextest/Base;");
  ASSERT_NE(nullptr, base_cls);
  auto sub1_cls = find_class_named(classes, "Lcom/facebook/redextest/SubOne;");
  ASSERT_NE(nullptr, sub1_cls);
  auto sub2_cls = find_class_named(classes, "Lcom/facebook/redextest/SubTwo;");
  ASSERT_NE(nullptr, sub2_cls);
  auto sub3_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SubThree;");
  ASSERT_NE(nullptr, sub3_cls);

  // Verify invoke-virtual bindings.
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/ResolveRefsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testSimpleInvokeVirtual");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 base_cls->get_type()));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub1_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub2_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub3_cls->get_type()));
}

TEST_F(PostVerify, FactoryBaseInvokeVirtual) {
  auto base_cls = find_class_named(classes, "Lcom/facebook/redextest/Base;");
  ASSERT_NE(nullptr, base_cls);
  auto sub1_cls = find_class_named(classes, "Lcom/facebook/redextest/SubOne;");
  ASSERT_NE(nullptr, sub1_cls);
  auto sub2_cls = find_class_named(classes, "Lcom/facebook/redextest/SubTwo;");
  ASSERT_NE(nullptr, sub2_cls);
  auto sub3_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SubThree;");
  ASSERT_NE(nullptr, sub3_cls);

  // Verify invoke-virtual bindings.
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/ResolveRefsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testFactoryBaseInvokeVirtual");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 base_cls->get_type()));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub1_cls->get_type()));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub2_cls->get_type()));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub3_cls->get_type()));
}

TEST_F(PostVerify, FactoryCastInvokeVirtual) {
  auto base_cls = find_class_named(classes, "Lcom/facebook/redextest/Base;");
  ASSERT_NE(nullptr, base_cls);
  auto sub1_cls = find_class_named(classes, "Lcom/facebook/redextest/SubOne;");
  ASSERT_NE(nullptr, sub1_cls);
  auto sub2_cls = find_class_named(classes, "Lcom/facebook/redextest/SubTwo;");
  ASSERT_NE(nullptr, sub2_cls);
  auto sub3_cls =
      find_class_named(classes, "Lcom/facebook/redextest/SubThree;");
  ASSERT_NE(nullptr, sub3_cls);

  // Verify invoke-virtual bindings.
  auto test_cls =
      find_class_named(classes, "Lcom/facebook/redextest/ResolveRefsTest;");
  ASSERT_NE(nullptr, test_cls);
  auto m = find_vmethod_named(*test_cls, "testFactoryCastInvokeVirtual");
  ASSERT_NE(nullptr, m);
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 base_cls->get_type()));
  ASSERT_EQ(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub1_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub2_cls->get_type()));
  ASSERT_NE(nullptr, find_invoke(m, DOPCODE_INVOKE_VIRTUAL, "foo",
                                 sub3_cls->get_type()));
}
