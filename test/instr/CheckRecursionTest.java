/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.List;
import org.junit.Test;
import static org.assertj.core.api.Assertions.assertThat;

public class CheckRecursionTest {
  // Positive test with bad self recursion.
  public void f1(int x, double y, double u, double v, List<Object> w) {
    f1(x, y, u, v, w);
    f1(x, y, u, v, w);
    f1(x, y, u, v, w);
    f1(x, y, u, v, w);
    w.add(g1(y, u, v));
  }

  public Object g1(double y, double u, double v) {
    return new Object();
  }

  // Negative test with self recurtsion that shoudn't be patched.
  public void f2(int x, double y, double u, double v, List<Object> w) {
    f2(x, y, u, v, w);
    f2(x, y, u, v, w);
    w.add(g2(y, u, v));
  }

  public Object g2(double y, double u, double v) {
    return new Object();
  }

  // Negative test with self recurtsion that shoudn't be patched.
  public void f3(int x, double y, double u, double v, List<Object> w) {
    f3(x, y, u, v, w);
    f3(x, y, u, v, w);
    f3(x, y, u, v, w);
    f3(x, y, u, v, w);
    try {
      w.add(g3(y, u, v));
    } catch (AnException e) {
      throw e;
    }
  }

  public Object g3(double y, double u, double v) {
    return new Object();
  }

  // Runtime test that checks that generated try/catch block works.
  final class AnException extends RuntimeException {
    public int val;
    AnException(int i) {
      val = i;
    }
  }

  public void bar(int i) {
    throw new AnException(i);
  }

  public void foo(int i) {
    switch (i) {
      case 1:
        foo(2);
        break;

      case 2:
        foo(3);
        break;

      case 3:
        foo(4);
        break;

      case 4:
        foo(5);
        break;

      default:
        bar(i);
    }
  }

  @Test
  public void throw_exception() {
    try {
      foo(1);
      assertThat(true).isEqualTo(false);
    } catch (AnException e) {
      assertThat(e.val).isEqualTo(5);
    }
  }
}
