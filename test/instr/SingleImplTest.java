/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import org.junit.Test;
import static org.assertj.core.api.Assertions.assertThat;

// TODO (fengliu): We may make the test tool to generate the PRECHECK directive
// automatically.
// PRECHECK: class: com.facebook.redextest.I_0
// POSTCHECK-NOT: class: com.facebook.redextest.I_0
interface I_0 {
  // I_0 and I_1 should not be optimized at the same time.
  I_1 get();
}

abstract class Impl_0 implements I_0 {}

// PRECHECK: class: com.facebook.redextest.I_1
// POSTCHECK-NOT: class: com.facebook.redextest.I_1
interface I_1 {
  // The interface contains a method which reference the interface itself.
  public I_1 get();
}

class Impl_1 implements I_1 {
  public Impl_1 get() { return this; }
}

// PRECHECK: class: com.facebook.redextest.I_2
// POSTCHECK-NOT: class: com.facebook.redextest.I_2
interface I_2 {
  public int get();
}

// PRECHECK: class: com.facebook.redextest.I_3
// POSTCHECK-NOT: class: com.facebook.redextest.I_3
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
