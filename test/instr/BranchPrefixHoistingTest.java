/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import java.util.Random;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.Test;

public class BranchPrefixHoistingTest {
  int getInt() {
    return new Random().nextInt();
  }

  // CHECK-LABEL: method: virtual redex.BranchPrefixHoistingTest.testPrefixHoisting
  @Test
  public void testPrefixHoisting() {
    // CHECK: invoke-virtual {{.*}} redex.BranchPrefixHoistingTest.getInt
    // CHECK: invoke-virtual {{.*}} redex.BranchPrefixHoistingTest.getInt
    // CHECK: invoke-virtual {{.*}} redex.BranchPrefixHoistingTest.getInt
    // CHECK: invoke-virtual {{.*}} redex.BranchPrefixHoistingTest.getInt
    int i = getInt();
    int j = getInt();
    int k = j;
    // All `getInt()` invocations are hoisted out of the branches.
    if (i > j) {
      // CHECK-NOT: invoke-virtual {{.*}} redex.BranchPrefixHoistingTest.getInt
      int i1 = getInt();
      int j1 = getInt();
      System.out.println(i1 + j1);
      i++;
      j++;
    } else {
      int i1 = getInt();
      int j1 = getInt();
      System.out.println(i1 + j1);
      i++;
      j++;
      System.out.println("else");
    }
    System.out.println(i + j);
    assertThat(j).isEqualTo(k + 1);
    // CHECK: return-void
  }
}
