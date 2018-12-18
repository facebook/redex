/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Show.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, MergeablesRemoval) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/A;");
  auto cls_b = find_class_named(classes, "Lcom/facebook/redextest/B;");
  auto cls_c = find_class_named(classes, "Lcom/facebook/redextest/C;");
  auto cls_d = find_class_named(classes, "Lcom/facebook/redextest/D;");
  verify_type_erased(cls_a);
  verify_type_erased(cls_b);
  verify_type_erased(cls_c);
  verify_type_erased(cls_d);
}
