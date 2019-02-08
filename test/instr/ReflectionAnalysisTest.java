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

  class Reflector {
    Class<? extends Foo> mClazz;
    String getClass(boolean flag) throws ClassNotFoundException {
      Class cls = null;
      if (flag) {
        cls = mClazz;
      } else {
        cls =
            Class.forName("com.facebook.redextest.ReflectionAnalysisTest$Foo");
        cls.getPackage();
      }
      return cls.getName();
    }
  }

  @Test
  public void testGetClass() {
    Reflector r = new Reflector();
    try {
      assertThat(r.getClass(false)).isNotNull();
    } catch (ClassNotFoundException ex) {
      // nothing
    }
  }
}
