/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class A {
  public int foo;
  public OnlyInArray[] arr;

  public A() {
    foo = 0;
    arr = new OnlyInArray[10];
  }

  public int bar() {
    return baz();
  }

  public int baz() {
    return 0;
  }

  // not referenced
  public void bor() {
  }
}

class D extends A {
  public int bar() { return 3; }
  public int baz() { return 4; }
}

interface I {
  public void wat();
}

class Super {
  public void wat() {}
}

class Sub extends Super implements I {
}

interface IBadger {
  boolean isAwesome();
}

abstract class Badger implements IBadger {
}

class HoneyBadger extends Badger {

  boolean mIsAwesome;

  public HoneyBadger(boolean isAwesome) {
    mIsAwesome = isAwesome;
  }

  @Override
  public boolean isAwesome() {
    return mIsAwesome;
  }
}

class HogBadger implements IBadger {
  @Override
  public boolean isAwesome() {
    return true;
  }
}

class BadgerTester {
  public static boolean testBadger(Badger b) {
    return b.isAwesome();
  }
}

interface IParent {
  public void go();
}

interface IChild extends IParent {
}

class UseIt {
  public static void go(IChild ic) {
    ic.go();
  }
}

// This example is distilled from Guava
interface Hasher {
  void putBytes();
  void putString();
}

abstract class AbstractHasher implements Hasher {
  public void putString() {
    putBytes();
  }
}

class TestHasher extends AbstractHasher {
  public void putBytes() {
  }
}

class UseHasher {
  public static void use(AbstractHasher ah) {
    ah.putString();
  }

  public static void test() {
    TestHasher h = new TestHasher();
    use(h);
  }
}

class OnlyInArray {}
