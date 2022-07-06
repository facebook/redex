/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.redex.reachable;

import static org.fest.assertions.api.Assertions.*;

import java.util.ArrayList;
import java.util.List;
import java.lang.reflect.Method;

import com.facebook.redex.test.instr.KeepForRedexTest;

import org.junit.Test;

interface Duck {
  public int quack();
}

class A implements Duck {
  @Override
  public int quack() {
    return 100;
  }
}

class B implements Duck {

  public B(int x) {}

  @Override
  public int quack() {
    return 200;
  }
}

class C implements Duck {

  public C(int x) {}

  @Override
  public int quack() {
    return 300;
  }
}

class D implements Duck {

  public D(int x) {}

  public int quack() {
    return 400;
  }
}

// Let's leave this one unused.
class DD implements Duck {

  public DD(int x) {}

  @Override
  public int quack() {
    return 40000;
  }
}

class E implements Duck {

  public E(int x) {}

  @Override
  public int quack() {
    return 500;
  }
}

class Super {
  public void foo() {

  }

  private void bar() {

  }
}

class Sub extends Super {
  public void foo() {

  }

  private void foo(int x) {

  }

  public void bar() {

  }

  private void bar(int x) {

  }
}

@KeepForRedexTest
public class ReachableClassesTest {

  @Test
  @KeepForRedexTest
  public void testAccessClassesWithReflection() throws Exception {
    List<Integer> values = new ArrayList<>();

    // Constructors

    Class a = Class.forName("com.redex.reachable.A");
    Duck aa = (Duck) a.newInstance();
    values.add(aa.quack());

    Class b = Class.forName("com.redex.reachable.B");
    Duck bb = (Duck) b.getConstructor(int.class).newInstance(42);
    values.add(bb.quack());

    Class c = Class.forName("com.redex.reachable.C");
    Duck cc = (Duck) c.getDeclaredConstructor(int.class).newInstance(42);
    values.add(cc.quack());

    Class d = Class.forName("com.redex.reachable.D");
    Duck dd = (Duck) d.getConstructors()[0].newInstance(42);
    values.add(dd.quack());

    Class e = Class.forName("com.redex.reachable.E");
    Duck ee = (Duck) e.getDeclaredConstructors()[0].newInstance(42);
    values.add(ee.quack());

    // getMethod() vs getDeclaredMethod()

    // Should mark the public Sub.foo() and the Super.foo().
    Method foo = Class.forName("com.redex.reachable.Sub").getMethod("foo");
    // Should mark the public and private bar on Sub, but not the bar on Super.
    Method bar = Class.forName("com.redex.reachable.Sub").getDeclaredMethod("bar");

    assertThat(values).containsExactly(100, 200, 300, 400, 500);
  }
}
