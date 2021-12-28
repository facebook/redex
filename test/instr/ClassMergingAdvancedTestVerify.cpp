/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Show.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, MergerClassGenerated) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/A;");
  auto cls_b = find_class_named(classes, "Lcom/facebook/redextest/B;");
  auto cls_c = find_class_named(classes, "Lcom/facebook/redextest/C;");
  verify_type_erased(cls_a);
  verify_type_erased(cls_b);
  verify_type_erased(cls_c);
}

TEST_F(PostVerify, ClassWithStaticFields) {
  auto cls_d = find_class_named(classes, "Lcom/facebook/redextest/D;");
  verify_type_erased(cls_d, 1 /* clinit */);
  auto& static_fields = cls_d->get_sfields();
  ASSERT_EQ(static_fields.size(), 1);
}
