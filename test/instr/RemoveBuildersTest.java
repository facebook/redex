/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.instr;

import java.util.Random;

class Foo {
  private int x;

  public Foo(int x) {
    this.x = x;
  }

  public static class Builder {
    public int x;

    public Foo build() {
      return new Foo(this.x);
    }
  }
}

class FooMoreArguments {
  private int x;
  private int y;

  public FooMoreArguments(int x, int y) {
    this.x = x;
    this.y = y;
  }

  public static class Builder {
    public int x;
    public int y;

    public FooMoreArguments build() {
      return new FooMoreArguments(this.x, this.y);
    }
  }
}

class Bar {
  private int x;

  public Bar(int x) {
    this.x = x;
  }

  public static class Builder {
    public int x;

    public Bar build() {
      return new Bar(this.x);
    }
  }
}

class UsingNoEscapeBuilder {

  public Foo initializeFoo() {
    Foo.Builder builder = new Foo.Builder();
    builder.x = 3;
    return builder.build();
  }

  public FooMoreArguments initializeFooWithMoreArguments() {
    FooMoreArguments.Builder builder = new FooMoreArguments.Builder();
    builder.x = 3;
    builder.y = 4;
    return builder.build();
  }

  public Bar initializeBarDifferentRegs() {
    Random randomGen = new Random();
    Bar.Builder builder = new Bar.Builder();
    int value = randomGen.nextInt(10);
    if (value < 6) {
      builder.x = 6;
    } else {
      builder.x = 7;
    }

    return builder.build();
  }

  public Bar initializeBar() {
    Random randomGen = new Random();
    int value = randomGen.nextInt(10);
    if (value < 6) {
      Bar.Builder builder = new Bar.Builder();
      builder.x = 7;
      return builder.build();
    }

    return new Bar(value);
  }
}
