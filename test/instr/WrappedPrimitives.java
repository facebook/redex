/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex;

import java.util.Arrays;

class Constants {
  public static final long ONE = 1L;
  public static final long TWO = 2L;
  public static final long THREE = 3L;
  public static final long FOUR = 4L;
  public static final long FIVE = 5L;
  public static final long SIX = 6L;
  public static final long SEVEN = 7L;
  public static final long EIGHT = 8L;
  public static final long NINE = 9L;
}

class MyLong {
  final long value;
  public MyLong(long value) {
    this.value = value;
  }

  public static MyLong make(long value) {
    return new MyLong(value);
  }
};

class Bad {
  static void escape(MyLong l) {}
}

class AllValues {
  public static final MyLong L1 = MyLong.make(Constants.ONE);
  public static final MyLong L2 = new MyLong(Constants.TWO);
  public static final MyLong L3 = new MyLong(Constants.THREE);
  public static final MyLong L4 = new MyLong(Constants.FOUR);
  public static final MyLong L5 = new MyLong(System.currentTimeMillis());
  public static final MyLong L6 = new MyLong(System.currentTimeMillis() > 100 ? 101L : 100L);
  public static final MyLong L7;
  public static final MyLong L8 = new MyLong(Constants.EIGHT);

  static {
    MyLong defaultLong = new MyLong(666L);
    Bad.escape(L2);
    L7 = System.currentTimeMillis() > 100 ? new MyLong(Constants.SEVEN) : defaultLong;
  }
}

class Intermediate {
  public static final MyLong L8 = AllValues.L8;
}

class Intermediate2 {
  public static final MyLong L8 = Intermediate.L8;
}

class MoreValues {
  public static final MyLong L9;

  static {
    // This oddball case is important to guard against a field that is
    // uncontroversially understood as wrapping a known constant, but comes in
    // an atypical form with two sput-object instructions on the field.
    if (System.currentTimeMillis() > 100) {
      System.out.println("x");
      L9 = new MyLong(Constants.NINE);
    } else {
      System.out.println("y");
      L9 = new MyLong(9);
      Bad.escape(L9);
    }
  }
}

interface Unsafe {
  long getLong(long l);
  long peekLong(long l);
}

interface Safe {
  long getLong(MyLong l);
  long peekLong(MyLong l);
}

interface ThingToUse extends Safe {

}

class Receiver implements ThingToUse, Safe, Unsafe {
  public long getLong(MyLong l) {
    return l.value;
  }

  public long getLong(long l) {
    return l;
  }

  public long peekLong(MyLong l) {
    return l.value;
  }

  public long peekLong(long l) {
    return l;
  }

  public static class Logger {
    static void markFetched(MyLong l) {
    }
  }
}

public class WrappedPrimitives {

  private static final Object LOCK = new Object();

  // Larger method for actually running in instr test and asserting via JUnit
  public static long[] run() {
    long[] results = new long[9];
    Receiver r = new Receiver();
    results[0] = r.getLong(AllValues.L1);
    results[1] = r.peekLong(AllValues.L1);
    results[2] = r.getLong(AllValues.L2);
    MyLong local3 = AllValues.L3;
    results[3] = r.getLong(local3);
    Receiver.Logger.markFetched(local3);
    results[4] = r.getLong(AllValues.L4);
    results[5] = r.getLong(Intermediate.L8);
    results[6] = r.getLong(Intermediate2.L8);
    results[7] = r.getLong(MoreValues.L9);
    results[8] = r.getLong(System.currentTimeMillis() > 100 ? AllValues.L1 : AllValues.L2);
    return results;
  }

  // Smaller methods that are suitable for verifying via s-expr comparisons.
  public static long simple(Receiver r) {
    return r.getLong(AllValues.L1);
  }

  public static long simpleCast(ThingToUse t) {
    return t.getLong(AllValues.L1);
  }

  public static long multipleDefs(Receiver r) {
    return r.getLong(System.currentTimeMillis() > 100 ? AllValues.L1 : AllValues.L2);
  }

  // Another expected usage; Interface type is given, and will need a check-cast
  // to underlying impl which has the unwrapped method. Will need to ensure that
  // monitor-enter/exit instructions are properly balanced under such an
  // insertion
  public static synchronized long[] runMonitor(ThingToUse t) {
    long[] results = new long[1];
    long l;
    synchronized (LOCK) {
      l = t.getLong(AllValues.L1);
    }
    results[0] = l;
    return results;
  }

    public static synchronized long[] runAnother(ThingToUse t) {
    String tag = "X";
    long[] results = new long[1];
    try {
      results[0] = t.getLong(AllValues.L1);
    } catch (IllegalStateException e) {
      android.util.Log.w(tag, e);
    }
    return results;
  }

  public static long[] runWithInterface() {
    Receiver r = new Receiver();
    long[] one = runMonitor(r);
    long[] two = runAnother(r);
    long[] result = Arrays.copyOf(one, one.length + two.length);
    System.arraycopy(two, 0, result, one.length, two.length);
    return result;
  }
}
