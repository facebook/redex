/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class ConstructorDedupTest {
  public int f0;
  public boolean f1;
  public Object f2;

  public ConstructorDedupTest(int v0, boolean v1, Integer v2) {
    f0 = v0;
    f2 = v2;
    f1 = v1;
  }

  public ConstructorDedupTest(String v2, int v0, boolean v1) {
    f1 = v1;
    f2 = v2;
    f0 = v0;
  }

  public ConstructorDedupTest(boolean v1, int v0, String v2) {
    f0 = v0;
    f1 = v1;
    f2 = v2;
  }

  // The two constructors are the same but constructors with unused argument is not currently supported.
  public ConstructorDedupTest(boolean v1, String v2, int v0, int v3) {
    f0 = v0;
    f1 = v1;
    f2 = v2;
  }
  public ConstructorDedupTest(String v2, boolean v1, int v0, int v3) {
    f0 = v0;
    f2 = v2;
    f1 = v1;
  }

  public static void dedup_0() {
    ConstructorDedupTest obj1 = new ConstructorDedupTest(1, true, Integer.valueOf(1));
    ConstructorDedupTest obj2 = new ConstructorDedupTest("O", 2, false);
    ConstructorDedupTest obj3 = new ConstructorDedupTest(true, 3, "N");
  }

  public static void not_supported() {
    ConstructorDedupTest obj4 = new ConstructorDedupTest(true, "N", 4, 4);
    ConstructorDedupTest obj5 = new ConstructorDedupTest("N", true, 4, 4);
  }
}
