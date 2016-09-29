/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

import static org.fest.assertions.api.Assertions.*;

import java.lang.reflect.Modifier;
import java.lang.reflect.Method;
import org.junit.Test;

public class AccessMarkingTest {
  private static boolean isFinal(Class<?> cls) {
    return (cls.getModifiers() & Modifier.FINAL) != 0;
  }

  private static boolean isFinal(Method m) {
    return (m.getModifiers() & Modifier.FINAL) != 0;
  }

  @Test
  public void test() {
    Super f = null;
    assertThat(new Super().foo()).isEqualTo(1);
    assertThat(new Sub().foo()).isEqualTo(2);
    assertThat(new Super().bar()).isEqualTo(3);
    assertThat(new Sub().baz()).isEqualTo(4);
  }

  @Test
  public void testClassFinal() throws ClassNotFoundException {
    assertThat(isFinal(Super.class)).isFalse();
    assertThat(isFinal(Sub.class)).isTrue();
  }

  @Test
  public void testClassAbstract() {
    assertThat(isFinal(Abstract.class)).isFalse();
  }

  public void testMethodFinal() throws NoSuchMethodException {
    assertThat(isFinal(Super.class.getDeclaredMethod("foo"))).isFalse();
    assertThat(isFinal(Super.class.getDeclaredMethod("bar"))).isTrue();
    assertThat(isFinal(Sub.class.getDeclaredMethod("foo"))).isTrue();
    assertThat(isFinal(Sub.class.getDeclaredMethod("baz"))).isTrue();
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

abstract class Abstract {
}
