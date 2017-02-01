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

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.lang.reflect.Method;
import org.junit.Test;

public class AccessMarkingTest {
  private static boolean isFinal(Class<?> cls) {
    return (cls.getModifiers() & Modifier.FINAL) != 0;
  }

  private static boolean isFinal(Field f) {
    return (f.getModifiers() & Modifier.FINAL) != 0;
  }

  private static boolean isFinal(Method m) {
    return (m.getModifiers() & Modifier.FINAL) != 0;
  }

  private static boolean isStatic(Method m) {
    return (m.getModifiers() & Modifier.STATIC) != 0;
  }

  private static boolean isPrivate(Method m) {
    return (m.getModifiers() & Modifier.PRIVATE) != 0;
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

  @Test
  public void testMethodFinal() throws NoSuchMethodException {
    assertThat(isFinal(Super.class.getDeclaredMethod("foo"))).isFalse();
    assertThat(isFinal(Sub.class.getDeclaredMethod("foo"))).isTrue();
  }

  @Test
  public void testMethodStatic() throws NoSuchMethodException {
    Method bar = Super.class.getDeclaredMethod("bar");
    Method baz = Sub.class.getDeclaredMethod("baz");
    assertThat(isStatic(bar)).isTrue();
    assertThat(isFinal(bar)).isTrue();
    assertThat(isStatic(baz)).isTrue();
    assertThat(isFinal(baz)).isTrue();
  }

  @Test
  public void testMethodAbstract() throws NoSuchMethodException {
    Method nope = Abstract.class.getDeclaredMethod("nope");
    assertThat(isStatic(nope)).isFalse();
    assertThat(isFinal(nope)).isFalse();
  }

  @Test
  public void testCallStaticThroughSub() {
    assertThat(new Sub().bar()).isEqualTo(3);
  }

  @Test
  public void testPrivate() throws NoSuchMethodException {
    assertThat(new Doubler(4).get()).isEqualTo(8);
    Method doubleit = Doubler.class.getDeclaredMethod("doubleit");
    assertThat(isPrivate(doubleit)).isTrue();
  }

  @Test
  public void testFinalFields() throws NoSuchFieldException {
    Field f0 = FinalFixture.class.getDeclaredField("f0");
    Field f1 = FinalFixture.class.getDeclaredField("f1");
    Field f2 = FinalFixture.class.getDeclaredField("f2");
    Field f3 = FinalFixture.class.getDeclaredField("f3");
    Field f4 = FinalFixture.class.getDeclaredField("f4");
    assertThat(isFinal(f0)).isTrue();
    assertThat(isFinal(f1)).isTrue();
    assertThat(isFinal(f2)).isTrue();
    assertThat(isFinal(f3)).isFalse();
    assertThat(isFinal(f4)).isFalse();
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

class FinalFixture {
  public static final int f0 = 0;
  public static int f1 = 1;
  public static int f2;
  public static int f3;
  public static int f4;
  static {
    f2 = 2;
    if (Math.random() > 0.5) {
      f3 = 3;
    } else {
      f3 = 3;
    }
  }
}

class OtherFinalFixture {
  static {
    FinalFixture.f4 = 4;
  }
}

abstract class Abstract {
  abstract int nope();
}
