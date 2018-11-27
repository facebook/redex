/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Show.h"
#include "verify/VerifyUtil.h"

void verify_mergeable_post(const DexClass* cls) {
  ASSERT_NE(cls, nullptr);
  auto dmethods = cls->get_dmethods();
  for (auto m : dmethods) {
    ASSERT_FALSE(is_init(m));
    ASSERT_NE(m->c_str(), "<init>");
  }
  auto vmethods = cls->get_vmethods();
  ASSERT_TRUE(vmethods.empty());
}

TEST_F(PostVerify, MergeablesRemoval) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/A;");
  auto cls_b = find_class_named(classes, "Lcom/facebook/redextest/B;");
  auto cls_c = find_class_named(classes, "Lcom/facebook/redextest/C;");
  auto cls_d = find_class_named(classes, "Lcom/facebook/redextest/D;");
  verify_mergeable_post(cls_a);
  verify_mergeable_post(cls_b);
  verify_mergeable_post(cls_c);
  verify_mergeable_post(cls_d);
}
