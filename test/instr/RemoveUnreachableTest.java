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

public class RemoveUnreachableTest {
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
