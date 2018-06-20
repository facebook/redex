/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package redex;

import static org.fest.assertions.api.Assertions.*;

import java.lang.reflect.Modifier;
import java.lang.reflect.Method;
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

class Super {
  public int foo() { return 1; }
  public int bar() { return 3; }
}

class Sub extends Super {
  public int foo() { return 2; }
  public int baz() { return 4; }
}

class Doubler {
  private int mX;
  Doubler(int x) { mX = x; }
  public void doubleit() { mX *= 2; }
  public int get() { doubleit(); return mX; }
}

abstract class Abstract {
  abstract int nope();
}
