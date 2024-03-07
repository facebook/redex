/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import org.junit.Test;
import com.facebook.redex.ConstClassBranches;
import static org.assertj.core.api.Assertions.assertThat;

public class ConstClassBranchesTest {
  @Test
  public void testTranformableLookup() {
    assertThat(ConstClassBranches.A.get(java.util.Map.class)).isEqualTo(1000);
    assertThat(ConstClassBranches.A.get(java.util.List.class)).isEqualTo(1001);
    assertThat(ConstClassBranches.A.get(java.util.Set.class)).isEqualTo(1002);
    assertThat(ConstClassBranches.A.get(java.util.Deque.class)).isEqualTo(1003);
    assertThat(ConstClassBranches.A.get(java.util.Iterator.class)).isEqualTo(1004);
    assertThat(ConstClassBranches.A.get(java.util.Collection.class)).isEqualTo(1005);
    assertThat(ConstClassBranches.A.get(java.lang.String.class)).isNull();
    assertThat(ConstClassBranches.A.get(null)).isNull();
  }

  @Test
  public void testNonTranformableLookup() {
    assertThat(ConstClassBranches.B.get(java.util.Map.class)).isEqualTo(1000);
    assertThat(ConstClassBranches.B.get(java.util.List.class)).isEqualTo(1001);
    assertThat(ConstClassBranches.B.get(java.util.Set.class)).isEqualTo(1002);
    assertThat(ConstClassBranches.B.get(java.util.Deque.class)).isEqualTo(1003);
    assertThat(ConstClassBranches.B.get(java.util.Iterator.class)).isEqualTo(1026);
    assertThat(ConstClassBranches.B.get(java.util.Collection.class)).isEqualTo(1005);
    assertThat(ConstClassBranches.B.get(java.lang.String.class)).isNull();
    assertThat(ConstClassBranches.B.get(null)).isNull();
  }

  @Test
  public void testDuplicateLookup() {
    // The following class either needs to not get transformed, or get
    // transformed in a way that does not break.
    assertThat(ConstClassBranches.Duplicates.get(java.util.Map.class)).isEqualTo(1000);
    assertThat(ConstClassBranches.Duplicates.get(java.util.List.class)).isEqualTo(1001);
    assertThat(ConstClassBranches.Duplicates.get(java.util.Set.class)).isEqualTo(1002);
    assertThat(ConstClassBranches.Duplicates.get(java.util.Deque.class)).isEqualTo(1003);
    assertThat(ConstClassBranches.Duplicates.get(java.util.Iterator.class)).isEqualTo(1004);
    assertThat(ConstClassBranches.Duplicates.get(java.util.Collection.class)).isEqualTo(1005);
    assertThat(ConstClassBranches.Duplicates.get(java.lang.String.class)).isNull();
    assertThat(ConstClassBranches.Duplicates.get(null)).isNull();
  }

  @Test
  public void testMultipleTransformsInMethod() {
    String s = "a";
    assertThat(ConstClassBranches.Complicated.get(java.util.Map.class, s)).isEqualTo(1000);
    assertThat(ConstClassBranches.Complicated.get(java.util.List.class, s)).isEqualTo(1);
    assertThat(ConstClassBranches.Complicated.get(java.util.Set.class, s)).isEqualTo(2);
    assertThat(ConstClassBranches.Complicated.get(java.util.Deque.class, s)).isEqualTo(3);
    assertThat(ConstClassBranches.Complicated.get(java.util.Iterator.class, s)).isEqualTo(4);
    assertThat(ConstClassBranches.Complicated.get(java.util.Collection.class, s)).isEqualTo(5);
    assertThat(ConstClassBranches.Complicated.get(java.lang.String.class, s)).isNull();
    assertThat(ConstClassBranches.Complicated.get(null, s)).isNull();
    s = "b";
    assertThat(ConstClassBranches.Complicated.get(java.util.Date.class, s)).isEqualTo(1000);
    assertThat(ConstClassBranches.Complicated.get(java.util.List.class, s)).isEqualTo(1001);
    assertThat(ConstClassBranches.Complicated.get(java.util.Set.class, s)).isEqualTo(1002);
    assertThat(ConstClassBranches.Complicated.get(java.util.Deque.class, s)).isEqualTo(1003);
    assertThat(ConstClassBranches.Complicated.get(java.util.Iterator.class, s)).isEqualTo(1004);
    assertThat(ConstClassBranches.Complicated.get(java.util.Collection.class, s)).isEqualTo(1005);
    assertThat(ConstClassBranches.Complicated.get(java.lang.String.class, s)).isNull();
    assertThat(ConstClassBranches.Complicated.get(null, s)).isNull();
  }
}
