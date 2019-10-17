/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

class TestA {
  public int a;
  public int b;

  public TestA() {
    a = 0;
    b = 1;
  }

  public TestA(int param) { b = param; }
}

class TestB {
  public int a;
  public int b;

  public void change_ifield() {
    double random = Math.random();
    if (random > 1) {
      a = 0;
      b = 1;
    } else {
      a = 0;
      b = 0;
    }
  }
}

class GetItem {
  // CHECK: method: direct redex.GetItem.get_item
  public static int get_item(int input) {
    // CHECK-NOT: const{{.*}} #int 9
    // CHECK: const{{.*}} #int 3
    // CHECK: if-ge {{.*}}
    if (input < 3) {
      // CHECK: const{{.*}} #int 5
      return 5;
    }
    // CHECK: const{{.*}} #int 10
    return 10;
    // CHECK: return {{.*}}
  }
}

class TestC {
  public static int a = GetItem.get_item(2);
  public int another_call() { return GetItem.get_item(9); }
}

public class IPConstantPropagationTest {

  // CHECK: method: virtual redex.IPConstantPropagationTest.two_ctors
  @Test
  public void two_ctors() {
    TestA one = new TestA();
    // PRECHECK: iget {{.*}} redex.TestA.a:int
    // POSTCHECK-NOT: iget {{.*}} redex.TestA.a:int
    assertThat(one.a).isEqualTo(0);
    // CHECK: iget {{.*}} redex.TestA.b:int
    assertThat(one.b).isEqualTo(1);
    TestA two = new TestA(0);
    // PRECHECK: iget {{.*}} redex.TestA.a:int
    // POSTCHECK-NOT: iget {{.*}} redex.TestA.a:int
    assertThat(two.a).isEqualTo(0);
    // CHECK: iget {{.*}} redex.TestA.b:int
    assertThat(two.b).isEqualTo(0);
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.modified_elsewhere
  @Test
  public void modified_elsewhere() {
    TestB one = new TestB();
    one.change_ifield();
    // PRECHECK: iget {{.*}} redex.TestB.a:int
    // POSTCHECK-NOT: iget {{.*}} redex.TestB.a:int
    assertThat(one.a).isEqualTo(0);
    // CHECK: iget {{.*}} redex.TestB.b:int
    assertThat(one.b).isEqualTo(0);
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.call_by_clinit
  @Test
  public void call_by_clinit() {
    TestC c = new TestC();
    assertThat(c.another_call()).isEqualTo(10);
    // CHECK: return-void
  }
}
