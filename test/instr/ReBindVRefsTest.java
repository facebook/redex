/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest.rebind;

import java.util.*;
import org.junit.Test;
import static org.fest.assertions.api.Assertions.assertThat;

interface RootInterface {
  int bar();
}

interface BodyInterface {
  int dar();
}

interface LeafInterface {
  int car();
}

class Root implements RootInterface {
  public int foo() { return 53; }

  public int bar() { return 123; }
}

class Body extends Root implements BodyInterface {
  public Body(int val) { fooRes = val; }

  public int foo() { return fooRes; }

  public int bar() { return fooRes; }

  public int car() { return 23; }

  public int dar() { return fooRes; }

  public int fooRes;
}

class Leaf extends Body implements LeafInterface {
  public Leaf(int val) { super(val); }

  public int car() { return fooRes; }

  public int dar() { return fooRes; }
}

public class ReBindVRefsTest {
  public Body get_random_body() { return new Body(43); }

  public BodyInterface get_body_or_leaf() {
    Random rand = new Random();
    int val = rand.nextInt(10);

    if (val < 5) {
      return new Body(34);
    } else {
      return new Leaf(34);
    }
  }

  @Test
  public void testInvokeInterfaceReplaced() {
    RootInterface body_cls = get_random_body();
    assertThat(body_cls.bar() == 43);

    LeafInterface leaf_cls = new Leaf(54);
    assertThat(leaf_cls.car() == 54);
  }

  @Test
  public void testInvokeInterfaceSkipped() {
    BodyInterface some_cls = get_body_or_leaf();
    assertThat(some_cls.dar() == 34);
  }

  @Test
  public void testInvokeVirtualReplaced() {
    Random rand = new Random();
    int val = rand.nextInt(10);

    Root body_cls = new Body(val);
    assertThat(body_cls.foo() == val);
  }

  @Test
  public void testInvokeVirtualSkipped() {
    Random rand = new Random();
    int val = rand.nextInt(10);

    Root leaf_cls = new Leaf(val);
    assertThat(leaf_cls.foo() == 53);
  }
}
