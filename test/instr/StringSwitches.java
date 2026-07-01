/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package foo;

class AnotherExample {
  public static String lookup(String key) {
    switch (key) {
      case "abc": return "first";
      case "xyz": return "second";
      default: return "not found";
    }
  }

  public static String sameDestBlock(String key) {
    switch (key) {
      case "abc":
      case "def":
      case "ghi":
      case "jkl":
      case "mno":
      case "pqr":
      case "stu":
      case "vwx":
      case "yz":
        return "yay";
      default:
        return "nay";
    }
  }

  public static int noStringSwitch(int x) {
    switch (x) {
      case 1: return 10;
      case 2: return 20;
      default: return 0;
    }
  }
}

class Example {
  public String big(String s) {
    switch (s) {
      case "one":
        return "1";
      case "two":
        return "2";
      case "three":
        return "3";
      case "four":
        return "4";
      case "five":
        return "5";
      case "six":
        return "6";
      case "seven":
        return "7";
      case "eight":
        return "8";
      case "nine":
        return "9";
      case "ten":
        return "10";
      case "eleven":
        return "11";
      case "twelve":
        return "12";
      default:
        return "not found";
    }
  }

  public String decoy(String s) {
    int x = s.hashCode();
    int y = -1;
    switch (x) {
      case 666:
        if (s.equals("one")) {
          y = 0;
        }
        break;
      case 115276:
        if (s.equals("two")) {
          y = 1;
        }
        break;
      case 110339486:
        if (s.equals("three")) {
          y = 2;
        }
        break;
      case 3149094:
        if (s.equals("four")) {
          y = 3;
        }
        break;
      case 3143346:
        if (s.equals("five")) {
          y = 4;
        }
        break;
      case 113890:
        if (s.equals("six")) {
          y = 5;
        }
        break;
      case 109330445:
        if (s.equals("seven")) {
          y = 6;
        }
        break;
      case 96505999:
        if (s.equals("eight")) {
          y = 7;
        }
        break;
      case 3381426:
        if (s.equals("nine")) {
          y = 8;
        }
        break;
      case 114717:
        if (s.equals("ten")) {
          y = 9;
        }
        break;
      case -1300557247:
        if (s.equals("eleven")) {
          y = 10;
        }
        break;
      case -860970343:
        if (s.equals("twelve")) {
          y = 11;
        }
        break;
    }
    switch (y) {
      case 0:
        return "1";
      case 1:
        return "2";
      case 2:
        return "3";
      case 3:
        return "4";
      case 4:
        return "5";
      case 5:
        return "6";
      case 6:
        return "7";
      case 7:
        return "8";
      case 8:
        return "9";
      case 9:
        return "10";
      case 10:
        return "11";
      case 11:
        return "12";
      default:
        return "everything else";
    }
  }

  public String explicit(String s) {
    int x = s.hashCode();
    int y = -1;
    switch (x) {
      case 110182:
        if (s.equals("one")) {
          y = 0;
        }
        break;
      case 115276:
        if (s.equals("two")) {
          y = 1;
        }
        break;
      case 110339486:
        if (s.equals("three")) {
          y = 2;
        }
        break;
      case 3149094:
        if (s.equals("four")) {
          y = 3;
        }
        break;
      case 3143346:
        if (s.equals("five")) {
          y = 4;
        }
        break;
      case 113890:
        if (s.equals("six")) {
          y = 5;
        }
        break;
      case 109330445:
        if (s.equals("seven")) {
          y = 6;
        }
        break;
      case 96505999:
        if (s.equals("eight")) {
          y = 7;
        }
        break;
      case 3381426:
        if (s.equals("nine")) {
          y = 8;
        }
        break;
      case 114717:
        if (s.equals("ten")) {
          y = 9;
        }
        break;
      case -1300557247:
        if (s.equals("eleven")) {
          y = 10;
        }
        break;
      case -860970343:
        if (s.equals("twelve")) {
          y = 11;
        }
        break;
    }
    switch (y) {
      case 0:
        return "1";
      case 1:
        return "2";
      case 2:
        return "3";
      case 3:
        return "4";
      case 4:
        return "5";
      case 5:
        return "6";
      case 6:
        return "7";
      case 7:
        return "8";
      case 8:
        return "9";
      case 9:
        return "10";
      case 10:
        return "11";
      case 11:
        return "12";
      default:
        return "everything else";
    }
  }

  public String handleMultiple(int x, String s) {
    if (x >= 100) {
      switch (s) {
        case "one hundred":
          return "100";
        case "two hundred":
          return "200";
        case "three hundred":
          return "300";
        default:
          return "something else";
      }
    } else {
      switch (s) {
        case "one":
          return "1";
        case "two":
          return "2";
        case "three":
          return "3";
        default:
          return "everything else";
      }
    }
  }

  public String handleValue(String s) {
    switch (s) {
      case "one":
        return "1";
      case "two":
        return "2";
      case "three":
        return "3";
      default:
        return "everything else";
    }
  }

  public String handleWithCollisions(String s) {
    switch (s) {
      case "FB":
        return "a";
      case "Ea":
        return "b";
      case "something else":
        return "c";
      default:
        return "d";
    }
  }

  public String minimal(String s) {
    switch (s) {
      case "one":
        return "1";
      default:
        return "everything else";
    }
  }

  public String wrappedInTryCatchMulti(String s) {
    try {
      switch (s) {
        case "one":
          return "1";
        case "two":
          return "2";
        case "three":
          return "3";
        case "four":
          return "4";
        case "five":
          return "5";
        case "six":
          return "6";
        case "seven":
          return "7";
        case "eight":
          return "8";
        case "nine":
          return "9";
        case "ten":
          return "10";
        case "eleven":
          return "11";
        case "twelve":
          return "12";
        default:
          return "not found";
      }
    } catch (OutOfMemoryError o) {
      return "out of memory";
    } catch (Exception e) {
      return "something else...";
    }
  }

  public String wrappedInTryCatch(String s) {
    try {
      switch (s) {
        case "one":
          return "1";
        case "two":
          return "2";
        case "three":
          return "3";
        case "four":
          return "4";
        case "five":
          return "5";
        case "six":
          return "6";
        case "seven":
          return "7";
        case "eight":
          return "8";
        case "nine":
          return "9";
        case "ten":
          return "10";
        case "eleven":
          return "11";
        case "twelve":
          return "12";
        default:
          return "not found";
      }
    } catch (Exception e) {
      return "uh oh...";
    }
  }

  // A real source-level String switch whose case bodies ALSO call s.hashCode().
  // Exercises that (a) d8/CSE may elide the case-body hashCode() in favor of the
  // switch's already-computed (immutable) result, and (b) the extra hashCode()
  // calls -- which the driver also anchors on -- do not produce spurious
  // switches. Recovered as a HASH_SWITCH with 3 string cases + default (4
  // total), and nothing else.
  public int switchWithHashInBodies(String s) {
    switch (s) {
      case "one":
        return s.hashCode() + 1;
      case "two":
        return s.hashCode() + 2;
      case "three":
        return s.hashCode() + 3;
      default:
        return s.hashCode();
    }
  }
}

// Degenerate 1-case String dispatches hand-written as `if (s.equals(lit))`
// rather than a source-level `switch`. After d8 desugaring these are
// structurally identical to a 1-case switch -- a single subject.equals(literal)
// test guarding the matched body, with the fall-through as the default -- so
// StringSwitchFinder recovers each as a 1-case EQUALS_CHAIN (2 cases: the
// literal + default). That is correct, NOT a false positive: they are
// materially the same as Example.minimal. The discarded leading hashCode() call
// mirrors the null-guard d8 emits for a real `switch (String)`.
class DegenerateStringSwitches {
  // A lone `s.equals(lit)`. Recovered as a 1-case EQUALS_CHAIN.
  public static String loneEquals(String s) {
    int h = s.hashCode();
    if (s.equals("foo")) {
      return "yes" + h;
    }
    return "no" + h;
  }

  // Negated test: `if (!s.equals(lit))`. The finder normalizes branch polarity,
  // so this is recovered as the same 1-case EQUALS_CHAIN (the literal maps to the
  // equal body, the fall-through is the default).
  public static String negatedEquals(String s) {
    int h = s.hashCode();
    if (!s.equals("foo")) {
      return "notfoo" + h;
    }
    return "foo" + h;
  }
}

// Genuine non-switches: StringSwitchFinder reports nothing for any method here.
// They probe soundness (no false positives) against shapes that resemble, but
// are not, a recoverable String switch.
class Counterexamples {
  // Several INDEPENDENT (non-exclusive) equals() checks that accumulate into a
  // StringBuilder. Not a switch -- every branch can run, and the bodies fall
  // through and merge back into the chain, so the chain blocks have predecessors
  // outside any clean switch region and recognition fails self-containment.
  public static String independentEquals(String s) {
    int h = s.hashCode();
    StringBuilder sb = new StringBuilder().append(h);
    if (s.equals("a")) {
      sb.append("A");
    }
    if (s.equals("b")) {
      sb.append("B");
    }
    if (s.equals("c")) {
      sb.append("C");
    }
    return sb.toString();
  }

  // A genuine int switch ON the hash value, with no equals() at all. The
  // hashCode()+equals() pre-filter rejects it before analysis (and Form A would
  // require the per-case equals regardless).
  public static int rawHashSwitch(String s) {
    switch (s.hashCode()) {
      case 1:
        return 100;
      case 2:
        return 200;
      default:
        return 0;
    }
  }

  // A hash switch with a hash-matching equals() but NO second (ordinal) switch.
  // Form A requires the second-stage ordinal switch, so recognition fails.
  public static int hashSwitchEqualsNoOrdinal(String s) {
    int r = 0;
    switch (s.hashCode()) {
      case 110182: // "one".hashCode()
        if (s.equals("one")) {
          r = 1;
        }
        break;
      default:
        break;
    }
    return r;
  }

  // Reversed operands: `lit.equals(s)`, i.e. the constant is the receiver and the
  // subject is the argument. The finder only matches subject.equals(literal)
  // (subject as the receiver), so this is not recognized.
  public static String constReceiverEquals(String s) {
    int h = s.hashCode();
    if ("foo".equals(s)) {
      return "yes" + h;
    }
    return "no" + h;
  }
}
