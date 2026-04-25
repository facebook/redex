/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

/**
 * Test class with a simple static initializer for ClinitBatchingPass testing.
 * The clinit only uses SPUT of constants to its own fields.
 */
public class ClinitBatchingTest {
    public static int sField1;
    public static int sField2;
    public static int sField3;
    public static int sField4;
    public static int sField5;
    public static int sField6;
    public static int sField7;
    public static int sField8;

    static {
        sField1 = 10;
        sField2 = 20;
        sField3 = 30;
        sField4 = 40;
        sField5 = 50;
        sField6 = 60;
        sField7 = 70;
        sField8 = 80;
    }

    public static int getValue() {
        return sField1;
    }

    public static int getInt() {
        return sField2;
    }
}

/**
 * Second test class with a pure clinit (only SPUT of constants to own fields).
 */
class ClinitBatchingTestB {
    public static int sBField1;
    public static int sBField2;
    public static int sBField3;
    public static int sBField4;
    public static int sBField5;
    public static int sBField6;
    public static int sBField7;
    public static int sBField8;

    static {
        sBField1 = 100;
        sBField2 = 200;
        sBField3 = 300;
        sBField4 = 400;
        sBField5 = 500;
        sBField6 = 600;
        sBField7 = 700;
        sBField8 = 800;
    }
}

/**
 * Third test class with a pure clinit (only SPUT of constants to own fields).
 */
class ClinitBatchingTestC {
    public static int sCField1;
    public static int sCField2;
    public static int sCField3;
    public static int sCField4;
    public static int sCField5;
    public static int sCField6;
    public static int sCField7;
    public static int sCField8;

    static {
        sCField1 = 1000;
        sCField2 = 2000;
        sCField3 = 3000;
        sCField4 = 4000;
        sCField5 = 5000;
        sCField6 = 6000;
        sCField7 = 7000;
        sCField8 = 8000;
    }
}

/**
 * Class with small clinit.
 */
class ClinitBatchingTestSmall {
    public static int sSmallField = 1;
}
