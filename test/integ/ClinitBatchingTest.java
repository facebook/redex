/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.Locale;

/**
 * Test class with a static initializer for ClinitBatchingPass testing.
 * The clinit should be identified as a candidate when marked as hot.
 */
public class ClinitBatchingTest {
    // Static field initialized in clinit
    public static String sField1;
    public static int sField2;
    public static double sField3;
    public static boolean sField4;

    static {
        // This static block creates a clinit method
        sField1 = "Hello";
        sField2 = 42;
        sField3 = 3.14159;
        sField4 = true;
        String temp = sField1 + " World";
        sField1 = temp;
        sField2 = sField2 * 2;
        sField3 = sField3 * 2;
        sField4 = !sField4;
    }

    public static String getValue() {
        return sField1;
    }

    public static int getInt() {
        return sField2;
    }
}

/**
 * Second test class with dependency on first class.
 */
class ClinitBatchingTestB {
    public static String sBField;

    static {
        // This clinit depends on ClinitBatchingTest
        sBField = ClinitBatchingTest.sField1 + " from B";
        String temp = sBField;
        sBField = temp + "!";
        sBField = sBField.toUpperCase(Locale.ROOT);
    }
}

/**
 * Third test class for dependency chain testing.
 */
class ClinitBatchingTestC {
    public static String sCField;

    static {
        // This clinit depends on ClinitBatchingTestB
        sCField = ClinitBatchingTestB.sBField + " from C";
        String temp = sCField;
        sCField = temp + "!!";
        sCField = sCField.toLowerCase(Locale.ROOT);
    }
}

/**
 * Class with small clinit.
 */
class ClinitBatchingTestSmall {
    public static int sSmallField = 1;
}
