/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex;

public class ConstClassBranches {

  public static int floop(int param) { return param; }

    public static class A {
    A() {}

    public static final Integer get(Class clazz) {
      if (clazz == java.util.Map.class) {
          return floop(1000);
      } else if (clazz == java.util.List.class) {
          return floop(1001);
      } else if (clazz == java.util.Set.class) {
          return floop(1002);
      } else if (clazz == java.util.Deque.class) {
          return floop(1003);
      } else if (clazz == java.util.Iterator.class) {
          return floop(1004);
      } else {
          return clazz == java.util.Collection.class ? floop(1005) : null;
      }
    }
  }


  public static class B {
    B() {}

    public static final Integer get(Class clazz) {
      if (clazz == java.util.Map.class) {
          return floop(1000);
      } else if (clazz == java.util.List.class) {
          return floop(1001);
      } else if (clazz == java.util.Set.class) {
          return floop(1002);
      } else if (clazz == java.util.Deque.class) {
          return floop(1003);
      } else {
        Class c = java.lang.StringBuilder.class;
        if (clazz == java.util.Iterator.class) {
          return floop(1004 + c.getName().substring(1).length());
        } else {
          return clazz == java.util.Collection.class ? floop(1005) : null;
        }
      }
    }
  }

  public static class Duplicates {
    Duplicates() {}

    public static final Integer get(Class clazz) {
      if (clazz == java.util.Map.class) {
          return floop(1000);
      } else if (clazz == java.util.List.class) {
          return floop(1001);
      } else if (clazz == java.util.Set.class) {
          return floop(1002);
      } else if (clazz == java.util.Deque.class) {
          return floop(1003);
      } else if (clazz == java.util.Iterator.class) {
          return floop(1004);
      } else if (clazz == java.util.Map.class) {
        return floop(999);
      } else {
          return clazz == java.util.Collection.class ? floop(1005) : null;
      }
    }
  }
}
