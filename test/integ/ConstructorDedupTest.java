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
  public Object f3;
  public Object f4;

  static class MergeableType1 {}

  static class MergeableType2 {}

  static class MergeableType3 {}

  static class MergeableType4 {}

  public ConstructorDedupTest(MergeableType1 v0, MergeableType2 v1, int v2) {
    f2 = v0;
    f0 = v2;
    f3 = v1;
  }

  public ConstructorDedupTest(MergeableType3 v0, MergeableType4 v1, int v2) {
    f2 = v0;
    f0 = v2;
    f3 = v1;
  }

  public ConstructorDedupTest(MergeableType1 v0, MergeableType3 v1, int v2) {
    f2 = v0;
    f0 = v2;
    f4 = v1;
  }

  public ConstructorDedupTest(MergeableType3 v0, MergeableType2 v1, int v2) {
    f2 = v0;
    f0 = v2;
    f4 = v1;
  }

  public ConstructorDedupTest(MergeableType3 v0, boolean v1, int v2) {
    f2 = v0;
    f0 = v2;
    f1 = v1;
  }

  public ConstructorDedupTest(MergeableType4 v0, boolean v1, int v2) {
    f2 = v0;
    f0 = v2;
    f1 = v1;
  }

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

  public static void dedup_1(
      MergeableType1 mt1, MergeableType2 mt2, MergeableType3 mt3, MergeableType4 mt4) {
    ConstructorDedupTest obj1 = new ConstructorDedupTest(mt1, mt2, 5);
    ConstructorDedupTest obj2 = new ConstructorDedupTest(mt3, mt4, 10);
  }

  public static void dedup_2(MergeableType1 mt1, MergeableType2 mt2, MergeableType3 mt3) {
    ConstructorDedupTest obj1 = new ConstructorDedupTest(mt1, mt3, 5);
    ConstructorDedupTest obj2 = new ConstructorDedupTest(mt3, mt2, 10);
  }

  public static void not_supported() {
    ConstructorDedupTest obj4 = new ConstructorDedupTest(true, "N", 4, 4);
    ConstructorDedupTest obj5 = new ConstructorDedupTest("N", true, 4, 4);
  }
}
