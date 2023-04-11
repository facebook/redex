/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import org.junit.Test;

import java.util.Objects;
import com.google.common.base.Preconditions;
import com.facebook.proguard.annotations.DoNotStrip;

class SampleObj {
  private final String s = "abc";
  public String getS() {
    return s;
  }
}

@DoNotStrip
class NullCheckConversionTest {
  private final String ss;
  private final SampleObj so;
  private final String so2;

  @DoNotStrip
  public NullCheckConversionTest(String ss, SampleObj so, String so2) {
    Objects.requireNonNull(ss);
    this.ss = ss;
    Objects.requireNonNull(this.ss);
    Objects.requireNonNull(so, "new bar must not be null");
    this.so = so;
    this.so2 = Preconditions.checkNotNull(so2);
    System.out.println(ss);
    System.out.println(so);
    System.out.println(so2);
  }

  public boolean Test2(String ss) {
    if (ss == null) {
      throw new NullPointerException();
    }
    System.out.println(ss);
    return true;
  }

  @DoNotStrip
  @Test
  public void Test(String[] args) {
    String s1 = "abc";
    NullCheckConversionTest my = new NullCheckConversionTest(s1, null, args[1]);
    System.out.println(my.Test2(args[1]));
  }
}
