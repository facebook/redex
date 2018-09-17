/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Show.h"
#include "verify/VerifyUtil.h"

void verify_mergeable_post(const DexClass* cls, size_t num_dmethods = 0) {
  ASSERT_NE(cls, nullptr);
  auto& dmethods = cls->get_dmethods();
  ASSERT_EQ(dmethods.size(), num_dmethods);
  auto& vmethods = cls->get_vmethods();
  ASSERT_TRUE(vmethods.empty());
}

TEST_F(PostVerify, MergerClassGenerated) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/A;");
  auto cls_b = find_class_named(classes, "Lcom/facebook/redextest/B;");
  auto cls_c = find_class_named(classes, "Lcom/facebook/redextest/C;");
  verify_mergeable_post(cls_a);
  verify_mergeable_post(cls_b);
  verify_mergeable_post(cls_c);
}

TEST_F(PostVerify, ClassWithStaticFields) {
  auto cls_d = find_class_named(classes, "Lcom/facebook/redextest/D;");
  verify_mergeable_post(cls_d, 1 /* clinit */);
  auto& static_fields = cls_d->get_sfields();
  ASSERT_EQ(static_fields.size(), 1);
}
