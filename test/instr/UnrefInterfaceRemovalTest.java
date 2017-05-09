/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * Interface removal
 */
interface Unused {}

interface A extends Unused {
  int doStuff();
}

class B implements A {
  int val = 42;
  public int doStuff() {
    return val;
  }
}

class C implements Unused {
  public int doStuff() {
    return 42;
  }
}

/**
 * Parser removal
 */
class EnclosingModels {
  public static class AModel {}
  public static class BModel {
    public static int b() { return 0; }
  }
}

class EnclosingParsers {
  public static class AParser {
    public static int doStuff() {
      return 42;
    }
  }
  public static class BParser {
    public static int b() {
      return 42;
    }
  }
}

public class UnrefInterfaceRemovalTest {

  @Test
  public void testSingleUnusedCase() {
    A a = new B();
    assert(a.doStuff() == 42);
    C c = new C();
    assert(c.doStuff() == 42);
  }

  @Test
  public void testParserRemoval() {
    assert(EnclosingParsers.AParser.doStuff() == 42);
    assert(EnclosingParsers.BParser.b() == 42);
  }
}
