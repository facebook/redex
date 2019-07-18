/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class RemoveUnusedFieldsTest {
  int unusedInt;
  String unusedString;

  public void init() {
    unusedInt = 1;
    unusedString = "foo";
  }
}
