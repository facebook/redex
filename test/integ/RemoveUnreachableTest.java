/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class A {
  public int foo;
  public OnlyInArray[] arr;

  static {
    // trivial
  }

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
  public void bor() {}
}

class D extends A {
  static {
    System.out.println("side effect");
  }

  public int bar() { return 3; }
  public int baz() { return 4; }
  public void bor() {}
}

interface I {
  public void wat();
}

class Super {
  public void wat() {}
}

class Sub extends Super implements I {
}

interface Badger {
  boolean isAwesome();
}

abstract class BadgerImpl implements Badger {
}

class HoneyBadger extends BadgerImpl {

  boolean mIsAwesome;

  public HoneyBadger(boolean isAwesome) {
    mIsAwesome = isAwesome;
  }

  @Override
  public boolean isAwesome() {
    return mIsAwesome;
  }
}

class HoneyBadgerInstantiated extends BadgerImpl {
  @Override
  public boolean isAwesome() {
    return true;
  }
}

class HogBadger implements Badger {
  @Override
  public boolean isAwesome() {
    return true;
  }
}

class BadgerTester {
  public static boolean testBadger(Badger b) {
    new HoneyBadgerInstantiated();
    return b.isAwesome();
  }
}

interface Parent {
  public void go();
}

interface Child extends Parent {
}

class UseIt {
  public static void go(Child ic) {
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

interface J {
  public void implementMe();
}

class Instantiated implements J {
  public void implementMe() {}
}
class Uninstantiated implements J {
  public Uninstantiated(int dummyParameterToAvoidInclusionOfDefaultArglessConstructor) { }
  public static J retainMe() { return new Instantiated(); }
  public void implementMe() {}
}

interface K {
  public void foo();
}
abstract class KImpl1Abstract implements K { }
class KImpl1Derived extends KImpl1Abstract {
  public void foo() { };
}
class KImpl2 implements K {
  public void foo() { };
}

interface ReferencedInterface {
  public void foo();
}
interface UnreferencedInterface extends ReferencedInterface {
}
class ClassImplementingUnreferencedInterface implements UnreferencedInterface {
  public void foo() {}
}


public class RemoveUnreachableTest {
  public static void testMethod() {
    // Inheritance test
    A d = new D();
    D d2 = (D) d;
    d2.bar();

    // Triangle inheritance
    I i = new Sub();
    i.wat();
  }

  public static void testUninstantiated() {
    J j = Uninstantiated.retainMe();
    j.implementMe();
  }

  public static void testSharpening() {
    KImpl1Abstract k = new KImpl1Derived();
    k.foo();
    new KImpl2();
  }

  public static void unreferencedInterface() {
    ReferencedInterface ri = new ClassImplementingUnreferencedInterface();
    ri.foo();
  }
}
