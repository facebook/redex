/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import android.app.Application;
import android.content.Context;
import com.facebook.redextest.annotation.GenerateStaticInitBatch;

/**
 * Minimal Application subclass for testing. The EarlyClassLoadsAnalysis
 * discovers this via the binary AndroidManifest.xml test fixture and uses
 * it as the entry point for the callgraph walk.
 */
class TestApplication extends Application {
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        ClinitBatchingOrchestrator.initAllStatics();
    }
}

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

/**
 * Orchestrator class for ClinitBatchingPass.
 * The annotated method will be filled with invoke-static calls to all
 * __initStatics$*() methods in dependency order.
 */
class ClinitBatchingOrchestrator {
    @GenerateStaticInitBatch
    public static void initAllStatics() {
        // Empty — ClinitBatchingPass fills this in.
    }
}

/**
 * Class with a simple clinit that only uses SPUT to its own fields.
 */
class SimpleClinitClass {
  static int s_value1 = 42;
  static int s_value2 = 43;
  static int s_value3 = 44;
  static int s_value4 = 45;
  static int s_value5 = 46;
  static int s_value6 = 47;
  static int s_value7 = 48;
  static int s_value8 = 49;
}

/**
 * Class A in a dependency chain. Only assigns to its own static fields.
 */
class DependencyClassA {
  static int s_a_value1 = 10;
  static int s_a_value2 = 11;
  static int s_a_value3 = 12;
  static int s_a_value4 = 13;
  static int s_a_value5 = 14;
  static int s_a_value6 = 15;
  static int s_a_value7 = 16;
  static int s_a_value8 = 17;
}

/**
 * Class B depends on A (reads A's static field via SGET).
 */
class DependencyClassB {
  static int s_b_value1 = DependencyClassA.s_a_value1 + 5;
  static int s_b_value2 = 20;
  static int s_b_value3 = 30;
  static int s_b_value4 = 40;
  static int s_b_value5 = 50;
  static int s_b_value6 = 60;
  static int s_b_value7 = 70;
  static int s_b_value8 = 80;
}
