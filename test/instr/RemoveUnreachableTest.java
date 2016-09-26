/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;
import java.util.concurrent.atomic.AtomicLongFieldUpdater;

public class RemoveUnreachableTest {
  volatile long counter;

  private static boolean classExists(String s) {
    try {
      Class.forName(s);
    } catch (ClassNotFoundException e) {
      return false;
    }
    return true;
  }

  @Test
  public void testSeeds() {
    assertThat(classExists("A")).isFalse();
  }

  /**
   * Inheritance test from Guava.
   */
  @Test
  public void testHasher() {
    TestHasher h = new TestHasher();
    UseHasher.use(h);
  }

  @Test
  public void testAtomicUpdater() {
    AtomicLongFieldUpdater<RemoveUnreachableTest> up
      = AtomicLongFieldUpdater.newUpdater(
          RemoveUnreachableTest.class,
          "counter");
  }

  @Test
  public void testFoo() {
    FooDoer.doFoo(new ActualFoo());
  }
}

class A {
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
}

interface IFoo {
  void foo();
}

abstract class BaseFoo implements IFoo {
}

abstract class SemiFoo extends BaseFoo {
}

class ActualFoo extends SemiFoo {
  @Override public void foo() {}
}

class FooDoer {
  public static void doFoo(SemiFoo sf) {
    sf.foo();
  }
}
