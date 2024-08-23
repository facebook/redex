/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex;

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
};

class Bad {
  static void escape(MyLong l) {}
}

class AllValues {
  public static final MyLong L1 = new MyLong(Constants.ONE);
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

class Receiver {
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
  public static long[] run() {
    long[] results = new long[8];
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
    return results;
  }
}
