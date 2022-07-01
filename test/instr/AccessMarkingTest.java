/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

public class AccessMarkingTest {

  @Test
  public void test() {
    Super f = null;
    assertThat(new Super().foo()).isEqualTo(1);
    assertThat(new Sub().foo()).isEqualTo(2);
    assertThat(new Super().bar()).isEqualTo(3);
    assertThat(new Sub().baz()).isEqualTo(4);
  }

  @Test
  public void testCallStaticThroughSub() {
    assertThat(new Sub().bar()).isEqualTo(3);
  }

  @Test
  public void testPrivate() throws NoSuchMethodException {
    assertThat(new Doubler(4).get()).isEqualTo(8);
  }
}

// CHECK-LABEL: class: redex.Super
// CHECK: Access flags: ()
// CHECK: Superclass: java.lang.Object
class Super {
  // FIXME: For some reason these two methods are not ordered by line number.
  // CHECK-DAG: method: virtual redex.Super.foo:()int (PUBLIC)
  public int foo() { return 1; }
  // CHECK-DAG: method: virtual redex.Super.bar:()int (PUBLIC, FINAL)
  public int bar() { return 3; }
}

// CHECK-LABEL: class: redex.Sub
// CHECK: Access flags: (FINAL)
// CHECK: Superclass: redex.Super
class Sub extends Super {
  // CHECK-DAG: method: virtual redex.Sub.foo:()int (PUBLIC, FINAL)
  public int foo() { return 2; }
  // CHECK-DAG: method: virtual redex.Sub.baz:()int (PUBLIC, FINAL)
  public int baz() { return 4; }
}

// CHECK-LABEL: class: redex.Doubler
class Doubler {
  private int mX;
  Doubler(int x) { mX = x; }
  // CHECK: method: direct redex.Doubler.doubleit:()void (PRIVATE, FINAL)
  public void doubleit() { mX *= 2; }
  public int get() { doubleit(); return mX; }
}

// CHECK-LABEL: class: redex.Abstract
// CHECK: Access flags: (ABSTRACT)
// CHECK: Superclass: java.lang.Object
abstract class Abstract {
  // CHECK: method: virtual redex.Abstract.nope:()int (ABSTRACT)
  abstract int nope();
}
