// Copyright 2004-present Facebook. All Rights Reserved.

package com.redex.reachable;

import static org.fest.assertions.api.Assertions.*;

import java.util.ArrayList;
import java.util.List;

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

@KeepForRedexTest
public class ReachableClassesTest {

  @Test
  @KeepForRedexTest
  public void testAccessClassesWithReflection() throws Exception {
    List<Integer> values = new ArrayList<>();

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

    assertThat(values).containsExactly(100, 200, 300, 400, 500);
  }
}
