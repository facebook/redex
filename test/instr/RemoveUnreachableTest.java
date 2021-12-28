/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import static org.fest.assertions.api.Assertions.assertThat;

import com.facebook.annotations.OkToExtend;
import com.facebook.redex.test.instr.KeepForRedexTest;
import org.junit.Test;

interface I {
  // PRECHECK: method: virtual com.facebook.redextest.I.bar:()int (PUBLIC, ABSTRACT)
  // POSTCHECK-NOT: method: virtual com.facebook.redextest.I.bar:()int (PUBLIC, ABSTRACT)
  public int bar();
}

@OkToExtend
class Base {
  // PRECHECK: method: virtual com.facebook.redextest.Base.foo:()java.lang.String
  // POSTCHECK-NOT: method: virtual com.facebook.redextest.Base.foo:()java.lang.String
  String foo() { return "Base"; }
}

@OkToExtend
class SubOne extends Base {
  @Override
  String foo() { return "SubOne"; }
}

@OkToExtend
class SubTwo extends Base {
  @Override
  String foo() { return "SubTwo"; }
}

class C1 implements I {
  @Override
  public int bar() {
    return 42;
  }
}

class C2 implements I {
  @Override
  public int bar() {
    return 43;
  }
}

@KeepForRedexTest
public class RemoveUnreachableTest {

  @KeepForRedexTest
  @Test
  public void testRebindInvokeVirtual() {
    // PRECHECK: invoke-virtual {{.*}} com.facebook.redextest.Base.foo
    // POSTCHECK: invoke-virtual {{.*}} com.facebook.redextest.SubOne.foo
    // POSTCHECK: invoke-virtual {{.*}} com.facebook.redextest.SubTwo.foo
    Base s1 = new SubOne();
    assertThat(s1.foo()).isEqualTo("SubOne");
    Base s2 = new SubTwo();
    assertThat(s2.foo()).isEqualTo("SubTwo");
  }

  @KeepForRedexTest
  @Test
  public void testReplaceInvokeInterface() {
    // PRECHECK: invoke-interface {{.*}} com.facebook.redextest.I.bar
    // POSTCHECK: invoke-virtual {{.*}} com.facebook.redextest.C1.bar
    // POSTCHECK: invoke-virtual {{.*}} com.facebook.redextest.C2.bar
    I c1 = new C1();
    assertThat(c1.bar()).isEqualTo(42);
    I c2 = new C2();
    assertThat(c2.bar()).isEqualTo(43);
  }
}
