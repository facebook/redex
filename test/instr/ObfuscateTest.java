/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.instr;

import static org.fest.assertions.api.Assertions.*;
import java.lang.reflect.*;

import org.junit.Test;

interface I1 {
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
  }
}
