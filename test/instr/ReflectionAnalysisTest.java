/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This Java class is used as a simple container for dynamically generated
 * methods.
 */

package com.facebook.redextest;

import org.junit.Test;
import java.lang.Integer;
import java.lang.reflect.Method;

import static org.fest.assertions.api.Assertions.assertThat;
import androidx.annotation.Nullable;

public class ReflectionAnalysisTest {
  private static Class[] sClassArray;

  class Foo {}
  class Bar {}
  class Baz {}

  class Reflector {
    Class<? extends Foo> mFooClazz;

    private Class[] mClassArray;

    String getClassJoinSame(boolean flag) throws ClassNotFoundException {
      Class cls = null;
      if (flag) {
        cls =
            Class.forName("com.facebook.redextest.ReflectionAnalysisTest$Foo");
      } else {
        cls =
            Foo.class;
      }
      return cls.getName();
    }

    String getClassJoinDifferent(boolean flag) throws ClassNotFoundException {
      Class cls = null;
      if (flag) {
        cls =
            Class.forName("com.facebook.redextest.ReflectionAnalysisTest$Foo");
      } else {
        cls =
            Class.forName("com.facebook.redextest.ReflectionAnalysisTest$Bar");
      }
      return cls.getName();
    }

    String getClassJoinEmpty(boolean flag) throws ClassNotFoundException {
      Class cls = null;
      if (flag) {
        cls = mFooClazz;
      } else {
        cls =
            Class.forName("com.facebook.redextest.ReflectionAnalysisTest$Foo");
        cls.getPackage();
      }
      return cls.getName();
    }

    Class getStringJoinSame(boolean flag) throws ClassNotFoundException {
      String str = null;
      if (flag) {
        str = "com.facebook.redextest.ReflectionAnalysisTest$Foo";
      } else {
        str = "com.facebook.redextest.ReflectionAnalysisTest$Foo";
      }
      return Class.forName(str);
    }

    Class getStringJoinDifferent(boolean flag) throws ClassNotFoundException {
      String str = null;
      if (flag) {
        str = "com.facebook.redextest.ReflectionAnalysisTest$Foo";
      } else {
        str = "com.facebook.redextest.ReflectionAnalysisTest$Bar";
      }
      return Class.forName(str);
    }

    Class getStringJoinEmpty(boolean flag) throws ClassNotFoundException {
      String str = null;
      if (flag) {
        str = "com.facebook.redextest.ReflectionAnalysisTest$Foo";
      } else {
        str = "";
      }
      return Class.forName(str);
    }

    @Nullable
    Method getMethodWithParam() {
      Class<?> baz = Baz.class;
      Class[] empty = new Class[0];
      Class[] types = new Class[2];
      types[0] = Integer.class;
      types[1] = double.class;
      Method m1 = null;
      Method m2 = null;
      Method m3 = null;
      try {
        m1 = baz.getDeclaredMethod("test", empty);
        m2 = baz.getDeclaredMethod("test", types);
        m3 = baz.getDeclaredMethod("test2", types);
      } catch (NoSuchMethodException e) { return null; }
      return m1;
    }

    private void updateClassArray(Class[] array) {}

    Method getMethodWithParamOriginal() throws NoSuchMethodException {
      Class<?> baz = Baz.class;
      Class[] types = new Class[2];
      types[0] = Integer.class;
      types[1] = double.class;
      return baz.getDeclaredMethod("test", types);
    }

    Method getMethodWithParamInvalidatedArgs1() throws NoSuchMethodException {
      Class<?> baz = Baz.class;
      Class[] types = new Class[2];
      types[0] = Integer.class;
      types[1] = double.class;
      mClassArray = types; // invalidate
      return baz.getDeclaredMethod("test", types);
    }

    Method getMethodWithParamInvalidatedArgs2() throws NoSuchMethodException {
      Class<?> baz = Baz.class;
      Class[] types = new Class[2];
      types[0] = Integer.class;
      types[1] = double.class;
      sClassArray = types; // invalidate
      return baz.getDeclaredMethod("test", types);
    }

    Method getMethodWithParamInvalidatedArgs3() throws NoSuchMethodException {
      Class<?> baz = Baz.class;
      Class[] types = new Class[2];
      types[0] = Integer.class;
      types[1] = double.class;
      updateClassArray(types); // invalidate
      return baz.getDeclaredMethod("test", types);
    }

    Method getMethodWithParamInvalidatedArgs4() throws NoSuchMethodException {
      Class<?> baz = Baz.class;
      Class[] types = new Class[2];
      types[0] = Integer.class;
      types[1] = double.class;
      Class[][] someArray = new Class[1][];
      someArray[0] = types; // invalidate
      return baz.getDeclaredMethod("test", types);
    }
  }

  @Test
  public void testGetClass() {
    Reflector r = new Reflector();
    try {
      assertThat(r.getClassJoinEmpty(false)).isNotNull();
      assertThat(r.getClassJoinSame(false)).isNotNull();
      assertThat(r.getClassJoinDifferent(false)).isNotNull();
      assertThat(r.getStringJoinEmpty(false)).isNotNull();
      assertThat(r.getStringJoinSame(false)).isNotNull();
      assertThat(r.getStringJoinDifferent(false)).isNotNull();
    } catch (ClassNotFoundException ex) {
      // nothing
    }
  }
}
