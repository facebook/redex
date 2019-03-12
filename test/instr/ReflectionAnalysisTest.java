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

import static org.fest.assertions.api.Assertions.assertThat;

public class ReflectionAnalysisTest {

  class Foo {}
  class Bar {}

  class Reflector {
    Class<? extends Foo> mFooClazz;

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
