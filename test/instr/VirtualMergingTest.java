/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import org.junit.Test;
import static org.fest.assertions.api.Assertions.assertThat;

/**
 * The following two classes test case of merging two virtual methods across two
 * classes.
 */
class ClassA {
  public int do_something() { return 42; }
}

class ClassB extends ClassA {
  public int do_something() { return 23; }
}

/**
 * The following two classes test case of merging a virtual methods into an
 * abstract method.
 */
abstract class ClassC {
  public abstract int do_something();
}

class ClassD extends ClassC {
  public int do_something() { return 23; }
}

/**
 * The following two classes test case of merging two virtual methods without
 * a result, but accessing a private field of the derived classes.
 */
class ClassE {
  public void do_something() { }
}

class ClassF extends ClassE {
  int b;
  public void do_something() { b += 42; }
  public int get() { return b; }
}

/**
 * The following four classes test merging of multiple virtual methods.
 */
class ClassG {
  public int do_something() { return 2; }
}

class ClassH extends ClassG {
  public int do_something() { return 27; }
}

class ClassI extends ClassH {
  public int do_something() { return 77; }
}

class ClassJ extends ClassH {
  public int do_something() { return -1; }
}

public class VirtualMergingTest {
  @Test
  public void testMergeAB() {
    ClassA a = new ClassA();
    ClassB b = new ClassB();
    assertThat(a.do_something()).isEqualTo(42);
    assertThat(b.do_something()).isEqualTo(23);
  }

  @Test
  public void testMergeCD() {
    ClassD d = new ClassD();
    assertThat(d.do_something()).isEqualTo(23);
  }

  @Test
  public void testMergeEF() {
    ClassF f = new ClassF();
    f.do_something();
    assertThat(f.get()).isEqualTo(42);
  }

  @Test
  public void testMergeGHIJ() {
    ClassG g = new ClassG();
    ClassH h = new ClassH();
    ClassI i = new ClassI();
    ClassJ j = new ClassJ();
    assertThat(g.do_something()).isEqualTo(2);
    assertThat(h.do_something()).isEqualTo(27);
    assertThat(i.do_something()).isEqualTo(77);
    assertThat(j.do_something()).isEqualTo(-1);
  }
}
