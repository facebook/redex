/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

// CHECK-LABEL: class: redex.RemoveUnusedFieldsTest
class RemoveUnusedFieldsTest {
  // NOTE: Be careful with `CHECK-NOT`. A typo can yield a false positive.
  // CHECK-NOT: redex.RemoveUnusedFieldsTest.unusedInt:int
  int unusedInt;
  // Removal of Strings are excluded in the test Redex config
  // CHECK: redex.RemoveUnusedFieldsTest.unusedString:java.lang.String
  String unusedString;

  public void init() {
    unusedInt = 1;
    unusedString = "foo";
  }
}
