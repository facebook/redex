/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

/**
 * RemoveUnusedClass checks to make sure a class that should be
 * removed is actually removed. The reflected class reference creation
 * below works fine if run as a Java program on the JVM because reflection
 * finds the the Zeta class. However, Redex should not be able to identify the
 * the Zeta class during the optization phase so it should be 'optimized'
 * out. This should then cause the ClassNotFoundException exception to
 * be thrown.
 * NOTE: This test is only of value when DelInit is turned on. Until
 * now the sense of thest is reversed i.e. isEqualTo(false). Once
 * DelInit is turned on we should flip this bit.
 */

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

class Zeta {
    private int zValue;

    public Zeta() {}

    public int doubleZeta() {
        return 2*zValue;
    }
}

public class RemoveUnusedClass {

    // To avoid the occurence of a string that contains
    // the fully qualified name of the Zeta class we break
    // up the construction of the class name string and use a ProGuard
    // configuration to prevent in-lining of these methods.
    private static String prefix() {
        return "com.facebook.redex.test.instr";
    }

    private static String suffix() {
        return "Zeta";
    }

    private static boolean classExists(String clsName) {
        try {
            Class.forName(clsName);
        } catch (ClassNotFoundException e) {
            return false;
        }
        return true;
    }

    @Test
    public void test() {
        assertThat(classExists("com.facebook.redex.test.instr")).isFalse();
    }
}
