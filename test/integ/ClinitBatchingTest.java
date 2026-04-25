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
    @SuppressWarnings("unused")
    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        int v = EarlyLoadedClass.sEarlyValue;
        ClinitBatchingOrchestrator.initAllStatics();
    }
}

/**
 * Test classes for ClinitBatchingPass integration tests.
 *
 * These classes have various clinit patterns that the pass should transform:
 * - Simple clinit with single sput
 * - Clinits with dependencies between classes
 * - An orchestrator method annotated with @GenerateStaticInitBatch
 */

// Simple class with a basic clinit.
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

// Class with multiple field initializations
class MultiFieldClinitClass {
  static String s_str = "hello";
  static int s_int = 100;
  static double s_double = 3.14;
}

// Class B depends on Class A (reads A's static field).
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

// DependencyClassB has SGET (reads A's field).
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

// Class C depends on Class B, which depends on Class A
class DependencyClassC {
  static int s_c_value = DependencyClassB.s_b_value1 + 3;
}

// Diamond dependency: D depends on B and E, both depend on A
class DependencyClassE {
  static int s_e_value = DependencyClassA.s_a_value1 * 2;
}

class DependencyClassD {
  static int s_d_value = DependencyClassB.s_b_value1 + DependencyClassE.s_e_value;
}

// Class with explicit static block
class ExplicitStaticBlockClass {
  static int s_computed;
  static {
    int temp = 0;
    for (int i = 0; i < 10; i++) {
      temp += i;
    }
    s_computed = temp;
  }
}

// Class with final static fields. The Java compiler encodes these as
// ConstantValue attributes rather than generating a <clinit>, so this class
// appears in the profile but has no clinit to transform.
class FinalFieldClass {
  static final int s_final_value = 999;
  static final String s_final_str = "final_string";
}

// Class that will contain the orchestrator method
class ClinitBatchingOrchestrator {
  @GenerateStaticInitBatch
  public static void initAllStatics() {
    // This method body should be replaced by Redex with
    // invoke-static calls to __initStatics$*() methods
  }
}

// Class with no clinit (should be ignored by the pass)
class NoClinitClass {
  int instanceField;

  public int getInstanceField() {
    return instanceField;
  }
}

// Wide fan-out dependency pattern: Base with 5 children depending on it
class WideFanOutBase {
  static int s_base_value = 100;
}

class WideFanOutChild1 {
  static int s_child1_value = WideFanOutBase.s_base_value + 1;
}

class WideFanOutChild2 {
  static int s_child2_value = WideFanOutBase.s_base_value + 2;
}

class WideFanOutChild3 {
  static int s_child3_value = WideFanOutBase.s_base_value + 3;
}

class WideFanOutChild4 {
  static int s_child4_value = WideFanOutBase.s_base_value + 4;
}

class WideFanOutChild5 {
  static int s_child5_value = WideFanOutBase.s_base_value + 5;
}

// Class with array fields
class ArrayFieldClass {
  static int[] s_int_array = new int[] {1, 2, 3, 4, 5};
  static String[] s_string_array = new String[] {"a", "b", "c"};
}

/**
 * Main test class with a simple static initializer for ClinitBatchingPass testing.
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

    public static void main(String[] args) {
        // Access static fields to ensure they are initialized
        System.out.println(SimpleClinitClass.s_value1);
        System.out.println(MultiFieldClinitClass.s_str);
        // Dependency chain classes
        System.out.println(DependencyClassA.s_a_value1);
        System.out.println(DependencyClassB.s_b_value1);
        System.out.println(DependencyClassC.s_c_value);
        System.out.println(DependencyClassD.s_d_value);
        System.out.println(DependencyClassE.s_e_value);
        System.out.println(ExplicitStaticBlockClass.s_computed);
        System.out.println(FinalFieldClass.s_final_value);
        // Wide fan-out classes
        System.out.println(WideFanOutChild1.s_child1_value);
        System.out.println(WideFanOutChild2.s_child2_value);
        System.out.println(WideFanOutChild3.s_child3_value);
        System.out.println(WideFanOutChild4.s_child4_value);
        System.out.println(WideFanOutChild5.s_child5_value);
        // Array field class
        System.out.println(ArrayFieldClass.s_int_array[0]);
        System.out.println(ArrayFieldClass.s_string_array[0]);
        // Constructor safety test classes
        System.out.println(SafeConstructorClass.s_obj.value);
        System.out.println(SafeConstructorWithSuperClass.s_obj.extra);
        System.out.println(SafeConstructorMultiFieldClass.s_obj1.value);
        System.out.println(UnsafeConstructorWithSgetClass.s_obj.value);
        System.out.println(UnsafeConstructorWithVirtualCallClass.s_obj.label);
        System.out.println(UnsafeConstructorWithStaticCallClass.s_obj.absValue);
        System.out.println(UnsafeInstantiatedClassClinitClass.s_obj.value);
        System.out.println(MixedSafeConstAndConstructorClass.s_obj.value);
    }
}

/**
 * Second test class (only SPUT of constants to own fields).
 * Used for candidate selection testing.
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
 * Third test class (only SPUT of constants to own fields).
 * Used for candidate selection testing.
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
 * Class loaded before the orchestrator in TestApplication.attachBaseContext.
 * Even though it's hot in the method profile (a batching candidate), the
 * EarlyClassLoadsAnalysis should detect that it's accessed before
 * initAllStatics() and exclude it from batching.
 */
class EarlyLoadedClass {
    public static int sEarlyValue;
    public static int sEarlyValue2;
    public static int sEarlyValue3;
    public static int sEarlyValue4;
    public static int sEarlyValue5;
    public static int sEarlyValue6;
    public static int sEarlyValue7;
    public static int sEarlyValue8;

    static {
        sEarlyValue = 42;
        sEarlyValue2 = 43;
        sEarlyValue3 = 44;
        sEarlyValue4 = 45;
        sEarlyValue5 = 46;
        sEarlyValue6 = 47;
        sEarlyValue7 = 48;
        sEarlyValue8 = 49;
    }
}
