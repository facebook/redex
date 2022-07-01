/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import java.util.Random;
import javax.annotation.Nullable;

class Foo {
  private int x;

  public Foo(int x) {
    this.x = x;
  }

  public static class Builder {
    public int x;

    public Builder() {
      x = 4;
    }

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

class Car {
  public @Nullable String model;
  public int version;

  public Car(int version) {
    this.version = version;
  }

  public Car(int version, String model) {
    this.version = version;
    this.model = model;
  }

  public static class Builder {
    public @Nullable String model;
    public int version;

    private void extraCallSetVersion(int version) {
      this.version = version;
    }

    public void setVersion(int version) {
      this.extraCallSetVersion(version);
    }

    public Car build() {
      return new Car(this.version, this.model);
    }
  }
}

class Dar {
  public int x;

  public Dar(int x) {
    this.x = x;
  }

  public static class Builder {
    public int x;

    public Dar build() {
      return new Dar(this.x);
    }
  }
}

class BPC {
  public int x;

  public BPC(Builder builder) {
    this.x = builder.x;
  }

  public static class Builder {
    public int x;

    public BPC build() {
      return new BPC(this);
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

  public Bar initializeBarDifferentBranchesSameValues() {
    Random randomGen = new Random();
    int value = randomGen.nextInt(10);
    Bar.Builder builder = new Bar.Builder();

    int x = 7;
    if (value < 6) {
      builder.x = x;
    } else if (value > 9) {
      builder.x = x;
    } else {
      builder.x = 91;
    }

    return builder.build();
  }

  public Car initializeNullCarModel(int version) {
    Car.Builder builder = new Car.Builder();
    builder.setVersion(version);
    Car car = new Car(builder.version);
    car.model = builder.model;

    return car;
  }

  public Car initializeNullOrDefinedCarModel(int version) {
    Car.Builder builder = new Car.Builder();
    builder.setVersion(version);

    Random randomGen = new Random();
    int value = randomGen.nextInt(10);
    if (value > 6) {
      builder.model = "random_model";
    }
    return builder.build();
  }

  /**
   * Since the builder instance is used in a conditional statement,
   * in order to remove it, we need to keep track of what initializing it
   * would mean in a context where the builder is removed.
   * This case is not treated for now.
   */
  public Dar initializeDar_KeepBuilder() {
    Random randomGen = new Random();
    int value = randomGen.nextInt(10);

    Dar.Builder builder = null;
    if (value > 7) {
      builder = new Dar.Builder();
      builder.x = 7;
    }

    if (builder != null) {
      return builder.build();
    }

    return new Dar(8);
  }

  public BPC initializeBPC() {
    BPC.Builder builder = new BPC.Builder();
    builder.x = 43;
    return builder.build();
  }
}
