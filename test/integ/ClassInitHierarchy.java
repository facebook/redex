/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest.classinit;

/**
 * Java classes to test the ClassInitCounter analysis upon.
 */
class Foo {
  public int field = 0;

  public static void staticEscape(Foo f) {}
  public void instanceEscape() {}
}

class Bar extends Foo {}
class Baz extends Foo {}
class Qux {}

public class ClassInitHierarchy {
  public static void main(String[] args) {
    Foo foo = new Foo();
    Foo.staticEscape(foo);

    Bar bar = new Bar();
    bar.instanceEscape();

    Baz baz = new Baz();
    baz.field = bar.field;

    Qux qux = new Qux();
  }
}
