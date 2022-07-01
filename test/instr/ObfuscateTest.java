/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.fest.assertions.api.Assertions.*;
import java.lang.reflect.*;

import org.junit.Test;

interface I1 {
    public static int f1[] = {0};
    public void m1();
}

abstract class A1 implements I1 {
}

abstract class A2 extends A1 {
}

class C1 extends A2 {
    public void m1() {
    }
}

public class ObfuscateTest {

  @Test
  public void test() {
      A2 o = new C1();
      o.m1();
      assertThat(C1.f1[0]).isEqualTo(0);
  }
}
