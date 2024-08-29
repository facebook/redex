/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.redextest;

class Instrumentation {
  public static void onMethodBegin(int x) {

  }
}

public class R {
  static final int[] one = {
    0x7f010000,
    0x7f010001,
    0x7f010002,
    0x7f010003,
  };
  static final int[] two = {
    0x7f020000,
    0x7f020001,
    0x7f020002,
    0x7f020003,
  };
  static final int[] three = {
    0x7f030000,
    0x7f030001
  };

  static final int[] six = {
    0x7f060000,
    0x7f060001,
    0x7f060002,
    0x7f060003,
    0x7f060004,
    0x7f060005,
    0x7f060006,
    0x7f060007,
    0x7f060008,
    0x7f060009
  };


  static class styleable {
    static final int[] four = {
      0x7f040000,
      0x7f040001
    };

    static int MyButton_something = 0;
    static int MyButton_thingy = 1;
    static int MyButton_whatever = 1;
  }

  // Has a funny name, but meant to pass the simple sniff check by name.
  static class styleable2 {
    static final int[] five = {
      0x7f050000,
      0x7f050001,
      0x7f050002,
      0x7f050003,
      0x7f050004,
      1,
      0x7f050006,
      0x7f050007,
      0x7f050008,
      0x7f050009
    };

    static {
      // Just to throw a wrench and make sure things still work ;)
      five[5] = 0x7f050005;
    }
  }

  // Also meant to pass the simple check to say this is styleable.
  static class styleable_sgets {
    static final int[] seven = com.redextest.R.styleable.four;

    static int MyButton_something = com.redextest.R.styleable.MyButton_something;
    static int MyButton_thingy = com.redextest.R.styleable.MyButton_thingy;
    static int MyButton_whatever = com.redextest.R.styleable.MyButton_whatever;
  }
}
