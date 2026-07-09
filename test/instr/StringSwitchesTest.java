/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// In package `foo` so it can reach the package-private classes under test in
// StringSwitches.java. These assertions verify that StringSwitchTransformPass
// preserves runtime semantics: each method must return the same value whether or
// not its String switch was re-encoded into a StringTreeSet lookup.
package foo;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;

import org.junit.Test;

public class StringSwitchesTest {

  // Example.big -- a 13-case (12 + default) HASH_SWITCH; transformed.
  @Test
  public void testBig() {
    Example e = new Example();
    assertEquals("1", e.big("one"));
    assertEquals("6", e.big("six"));
    assertEquals("12", e.big("twelve"));
    assertEquals("not found", e.big("zzz"));
  }

  // Example.handleValue -- exactly at min_cases (3 + default); transformed.
  @Test
  public void testHandleValue() {
    Example e = new Example();
    assertEquals("1", e.handleValue("one"));
    assertEquals("2", e.handleValue("two"));
    assertEquals("3", e.handleValue("three"));
    assertEquals("everything else", e.handleValue("nope"));
  }

  // Two independent switches in one method; both transformed.
  @Test
  public void testHandleMultiple() {
    Example e = new Example();
    assertEquals("100", e.handleMultiple(100, "one hundred"));
    assertEquals("300", e.handleMultiple(100, "three hundred"));
    assertEquals("something else", e.handleMultiple(100, "nope"));
    assertEquals("1", e.handleMultiple(5, "one"));
    assertEquals("3", e.handleMultiple(5, "three"));
    assertEquals("everything else", e.handleMultiple(5, "nope"));
  }

  // Hand-written hashCode/equals/ordinal shape; transformed.
  @Test
  public void testExplicit() {
    Example e = new Example();
    assertEquals("1", e.explicit("one"));
    assertEquals("12", e.explicit("twelve"));
    assertEquals("everything else", e.explicit("nope"));
  }

  // A decoy with a wrong hash bucket (`case 666` for "one"): NOT a recoverable
  // String switch, so it is left untouched and must keep its (quirky) behavior.
  @Test
  public void testDecoy() {
    Example e = new Example();
    assertEquals("2", e.decoy("two"));
    assertEquals("3", e.decoy("three"));
    // "one" hashes to 110182, not the decoy's 666, so no case matches.
    assertEquals("everything else", e.decoy("one"));
  }

  // Switch inside a single try/catch; transformed (exercises throw-edge
  // reattachment on real d8 output).
  @Test
  public void testWrappedInTryCatch() {
    Example e = new Example();
    assertEquals("1", e.wrappedInTryCatch("one"));
    assertEquals("12", e.wrappedInTryCatch("twelve"));
    assertEquals("not found", e.wrappedInTryCatch("nope"));
    assertEquals("uh oh...", e.wrappedInTryCatch(null));
  }

  // Switch inside a try with TWO catch handlers; transformed (both handler edges
  // must be reattached). The null case is the key exceptional-flow check: the
  // lookup's NPE must propagate to the matching handler IN ORDER -- skipping the
  // OutOfMemoryError handler (caught first) and landing in the Exception handler
  // (caught second). If reattachment dropped a handler or scrambled the order,
  // this would crash or return "out of memory".
  @Test
  public void testWrappedInTryCatchMulti() {
    Example e = new Example();
    assertEquals("1", e.wrappedInTryCatchMulti("one"));
    assertEquals("10", e.wrappedInTryCatchMulti("ten"));
    assertEquals("not found", e.wrappedInTryCatchMulti("nope"));
    assertEquals("something else...", e.wrappedInTryCatchMulti(null));
  }

  // Below min_cases (1 + default): NOT transformed, behavior unchanged.
  @Test
  public void testMinimal() {
    Example e = new Example();
    assertEquals("1", e.minimal("one"));
    assertEquals("everything else", e.minimal("nope"));
  }

  // Case bodies that also call s.hashCode(); transformed. Compute the expected
  // values at runtime so we don't hardcode hashCodes.
  @Test
  public void testSwitchWithHashInBodies() {
    Example e = new Example();
    assertEquals("one".hashCode() + 1, e.switchWithHashInBodies("one"));
    assertEquals("two".hashCode() + 2, e.switchWithHashInBodies("two"));
    assertEquals("three".hashCode() + 3, e.switchWithHashInBodies("three"));
    assertEquals("zzz".hashCode(), e.switchWithHashInBodies("zzz"));
  }

  // Below min_cases (2 + default): NOT transformed, behavior unchanged.
  @Test
  public void testLookup() {
    assertEquals("first", AnotherExample.lookup("abc"));
    assertEquals("second", AnotherExample.lookup("xyz"));
    assertEquals("not found", AnotherExample.lookup("q"));
  }

  // Many labels sharing a single body (one distinct destination); transformed.
  @Test
  public void testSameDestBlock() {
    assertEquals("yay", AnotherExample.sameDestBlock("abc"));
    assertEquals("yay", AnotherExample.sameDestBlock("yz"));
    assertEquals("nay", AnotherExample.sameDestBlock("zzz"));
  }

  // 1-case `if (s.equals(...))` EQUALS_CHAINs: below min_cases, untouched.
  @Test
  public void testDegenerate() {
    assertEquals("yes" + "foo".hashCode(), DegenerateStringSwitches.loneEquals("foo"));
    assertEquals("no" + "bar".hashCode(), DegenerateStringSwitches.loneEquals("bar"));
    assertEquals("foo" + "foo".hashCode(), DegenerateStringSwitches.negatedEquals("foo"));
    assertEquals(
        "notfoo" + "bar".hashCode(), DegenerateStringSwitches.negatedEquals("bar"));
  }

  // A null subject must still throw NPE after the rewrite (the lookup dereferences
  // the subject, mirroring `switch (null)`).
  @Test
  public void testNullSubjectThrows() {
    Example e = new Example();
    assertThrows(NullPointerException.class, () -> e.big(null));
  }
}
