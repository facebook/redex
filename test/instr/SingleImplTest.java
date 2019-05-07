/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import org.junit.Test;
import static org.fest.assertions.api.Assertions.assertThat;

interface I_1 {
  // The interface contains a method which reference the interface itself.
  public I_1 get();
}

class Impl_1 implements I_1 {
  public Impl_1 get() { return this; }
}

interface I_2 {
  public int get();
}

interface I_3 {
  public int get_3(I_2 obj);
}

class Parent {
  public int get() {
    return 9;
  }
}

class Child extends Parent implements I_2, I_3 {
  public int get_3(I_2 obj) { return 3; }
}

public class SingleImplTest {
  @Test
  public void test_bridge() {
    I_1 obj = new Impl_1();
    I_1 obj2 = obj.get();
    assertThat(obj).isEqualTo(obj2);
  }

  @Test
  public void test_impl_method_is_in_parent() {
    Child obj = new Child();
    assertThat(obj.get()).isEqualTo(9);
    assertThat(obj.get_3(obj)).isEqualTo(3);
  }
}
