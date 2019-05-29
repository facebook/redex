/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.Random;
import javax.annotation.Nullable;
import org.junit.Test;
import static org.fest.assertions.api.Assertions.assertThat;

// This enum class contains some user defined static methods and it's never
// being casted to other types. It will be transformed to a class with only
// static methods by OptimizeEnumsPass.
enum SCORE {
  ONE,
  TWO,
  THREE;

  public static @Nullable SCORE increase(SCORE score) {
    if (score == null) {
      return null;
    }
    switch (score) {
    case ONE:
      return TWO;
    case TWO:
    case THREE:
      return THREE;
    }
    return null;
  }
}

interface Intf {
  int make(SCORE score);
  int make(int i);
}

// Some usages of SCORE.
class A {
  // As parameter.
  public int ha(SCORE score) { return 1; }
}

abstract class B extends A implements Intf {
  // As return value.
  public SCORE haha() { return SCORE.THREE; }
  public int make(int i) { return i * 7; }
  int ha(int i) { return 11; }
}

class C extends B {
  // As static field.
  static SCORE s_score = SCORE.ONE;
  // As instance field.
  @Nullable SCORE i_score = SCORE.ONE;
  @Nullable Object i_obj = null;
  // As element of an array.
  static SCORE[][] array;

  public void set(SCORE score) { i_score = score; }
  public @Nullable SCORE get() { return i_score; }

  public int make(SCORE score) { return score.ordinal() * 5; }
}

// This enum class only contains several Enum objects.
// It will be optimized by OptimizeEnumsPass.
enum PURE_SCORE {
  ONE,
  TWO,
  THREE;
}

/* Some enums that are unsafe to be transformed. */
enum CAST_WHEN_RETURN {
  ONE;
  public static Enum method() { return ONE; }
}
enum CAST_THIS_POINTER {
  ONE;
  public static void cast_this_method() {
    EnumHelper.inlined_method(CAST_THIS_POINTER.ONE);
  }
}
enum CAST_PARAMETER {
  ONE;
  public static void method(Object o) {}
  public static void method() { method(ONE); }
}
enum USED_AS_CLASS_OBJECT {
  ONE;
  public static <T> void method(Class<T> c) {}
  public static void method() { method(USED_AS_CLASS_OBJECT.class); }
}
enum CAST_CHECK_CAST { ONE; }
enum CAST_ISPUT_OBJECT { ONE; }
enum CAST_APUT_OBJECT { ONE; }

enum ENUM_TYPE_1 { ONE; }
enum ENUM_TYPE_2 {
  ONE,
  TWO;
  public static void test_join() {
    Enum obj = null;
    Random random = new Random();
    int pos_rand = random.nextInt() & Integer.MAX_VALUE;
    int selector = pos_rand % 2;
    if (selector == 0) {
      obj = ENUM_TYPE_1.ONE;
    } else if (selector == 1 || selector == -1) {
      obj = ENUM_TYPE_2.TWO;
    } else if (selector > 1) {
      obj = Enum.valueOf(ENUM_TYPE_1.class, "NotReach");
    }
    // obj may be Enum, ENUM_TYPE_1 or ENUM_TYPE_2
    int res = obj.ordinal();
    if (selector == 0) {
      assertThat(res).isEqualTo(0);
    } else if (selector == 1) {
      assertThat(res).isEqualTo(1);
    }
  }
}

class EnumHelper {
  static void inlined_method(Enum e) { int a = e.hashCode(); }

  class Cache<T> {
    @Nullable T e = (T) null;
    public @Nullable T get() { return e; }
  }
  Cache<CAST_CHECK_CAST> mCache = new Cache();
  public CAST_CHECK_CAST check_cast_method() { return mCache.get(); }

  Object i_obj;
  static Enum s_obj;
  public void put_method() {
    i_obj = CAST_ISPUT_OBJECT.ONE;
    s_obj = CAST_ISPUT_OBJECT.ONE;
  }

  public void aput_method() {
    Object[] array = new Object[10];
    array[0] = CAST_APUT_OBJECT.ONE;
  }

  public static int my_ordinal(SCORE score) { return score.ordinal(); }

  public static <T> T notEscape(T obj) { return obj; }
}

/* Test cases */
public class EnumTransformTest {
  @Test
  public void test_interface() {
    C c = new C();
    // Method with the same name but different type of argument.
    assertThat(c.make(SCORE.TWO)).isEqualTo(5);
    assertThat(c.make(1)).isEqualTo(7);
    assertThat(c.ha(0)).isEqualTo(11);
    assertThat(c.ha(null)).isEqualTo(1);
    assertThat(c.haha().ordinal()).isEqualTo(SCORE.THREE.ordinal());
  }

  // SCORE.ordinal() is transformed.
  @Test
  public void test_increase() {
    SCORE score = SCORE.ONE;
    assertThat(score.ordinal()).isEqualTo(SCORE.ONE.ordinal());
    assertThat(score.ordinal()).isEqualTo(0);
    score = SCORE.increase(score);
    assertThat(score.ordinal()).isEqualTo(1);
    score = SCORE.increase(score);
    assertThat(score.ordinal()).isEqualTo(2);
    score = SCORE.increase(score);
    assertThat(score.ordinal()).isEqualTo(2);
    score = SCORE.increase(null);
    if (score != null) {
      assertThat(true).isEqualTo(false);
    }
  }

  // SCORE.equals(SCORE) is transformed.
  @Test
  public void test_equals() {
    SCORE a = SCORE.ONE;
    assertThat(a.equals(SCORE.ONE)).isEqualTo(true);
    SCORE b = SCORE.increase(a);
    assertThat(a.equals(b)).isEqualTo(false);
  }

  // SCORE.compareTo(SCORE) is transformed.
  @Test
  public void test_compareTo() {
    SCORE a = SCORE.ONE;
    SCORE b = SCORE.increase(a);
    assertThat(a.compareTo(b)).isEqualTo(-1);
    assertThat(b.compareTo(a)).isEqualTo(1);
    assertThat(a.compareTo(a)).isEqualTo(0);
  }

  // SCORE.values() is transformed.
  @Test
  public void test_values() {
    assertThat(SCORE.values().length).isEqualTo(3);
  }

  // NullPointerException.
  @Test(expected = NullPointerException.class)
  public void test_npe() {
    int a = EnumHelper.my_ordinal(null);
  }

  // [isa]get_object, [isa]put_object and new_array instructions are
  // transformed.
  @Test
  public void test_get_put() {
    C.s_score = SCORE.ONE;
    assertThat(C.s_score.ordinal()).isEqualTo(0);

    C c = new C();
    c.set(SCORE.TWO);
    assertThat(c.get().ordinal()).isEqualTo(1);

    SCORE[] scores = new SCORE[1];
    scores[0] = SCORE.THREE;
    assertThat(scores[0].ordinal()).isEqualTo(SCORE.THREE.ordinal());
  }

  // PURE_SCORE class is deleted and PURE_SCORE objects are replaced with
  // Integer objects.
  @Test
  public void test_pure_score() {
    PURE_SCORE a = PURE_SCORE.ONE;
    assertThat(a.ordinal()).isEqualTo(PURE_SCORE.ONE.ordinal());
  }

  @Test
  public void test_if() {
    C c = new C();
    if (c.i_obj == c.i_score) {
      assertThat(true).isEqualTo(false);
    }
  }

  @Test
  public void test_toString() {
    C c = new C();
    StringBuilder sb = new StringBuilder();
    c.i_score = null;
    sb.append(c.i_score);
    sb.append(SCORE.ONE.toString());
    sb.append(SCORE.TWO);
    sb.append(SCORE.THREE);
    assertThat(sb.toString()).isEqualTo("nullONETWOTHREE");
  }

  @Test(expected = NullPointerException.class)
  public void test_toString_throw() {
    C c = new C();
    c.i_score = null;
    c.i_score.toString();
  }

  @Test
  public void test_valueOf() {
    String name = SCORE.ONE.name();
    SCORE s = SCORE.valueOf(name);
    assertThat(s.ordinal()).isEqualTo(SCORE.ONE.ordinal());
    s = SCORE.valueOf("TWO");
    assertThat(s.ordinal()).isEqualTo(SCORE.TWO.ordinal());
    s = SCORE.valueOf("THREE");
    assertThat(s.ordinal()).isEqualTo(SCORE.THREE.ordinal());
  }

  @Test(expected = IllegalArgumentException.class)
  public void test_valueOf_exception() {
    SCORE.valueOf("ZERO");
  }

  @Test
  public void test_join_with_multitypes() {
    ENUM_TYPE_2.test_join();
  }

  @Test
  public void test_non_escape_invocation() {
    SCORE one = SCORE.ONE;
    SCORE obj = EnumHelper.notEscape(one);
    assertThat(one.ordinal()).isEqualTo(obj.ordinal());
  }
}
