/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import java.util.Random;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Before;
import org.junit.Test;

public class BranchPrefixHoistingTest {
  int getInt() {
    return new Random().nextInt();
  }

  long getLong() {
    return new Random().nextLong();
  }

  @Test
  public void testPrefixHoisting1() {
    int i = getInt();
    int j = getInt();
    int k = j;
    if (i > j) {
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
  }
}
