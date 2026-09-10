/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// @nolint ANDROIDLINT: Lint crashes on this file due to a bug with visitClassInitializer

package com.facebook.redextest;

import java.lang.annotation.Target;
import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/**
 * Annotation to mark classes that we expect to have clinits detected
 * as unconditionally throwing by the pass.
 *
 * Note: Java source code cannot easily create clinits that *directly*
 * throw unconditionally (the compiler requires static initializers to
 * be able to complete normally). However, this test infrastructure
 * can be used to verify the pass logic on synthetic bytecode or
 * real-world cases.
 */
@Target(ElementType.TYPE)
@Retention(RetentionPolicy.RUNTIME)
@interface UnconditionallyThrows {
}

/**
 * Class with no <clinit> - should NOT be flagged.
 */
class NoClinitClass {
    public static int value;
    public static void doSomething() {}
}

/**
 * Class with a normal <clinit> that just initializes static fields.
 * Should NOT be flagged as unconditionally throwing.
 */
class NormalClinitClass {
    static int value = 42;
    static String name = "test";
}

/**
 * Class whose <clinit> calls a method that throws.
 * The clinit itself invokes the method, so it doesn't directly throw.
 * The pass checks entry block throwing, so this should NOT be flagged.
 *
 * In real-world bytecode, classes that directly throw in clinit
 * (like classes with broken static initialization) would be flagged.
 */
class InvokeThrowingMethodClinitClass {
    static {
        throwAlways();
    }

    private static void throwAlways() {
        throw new RuntimeException("This method always throws");
    }
}

/**
 * Class whose <clinit> conditionally throws based on some condition.
 * Should NOT be flagged since it doesn't always throw.
 */
class ConditionalThrowClinitClass {
    static int value;
    static {
        if (System.nanoTime() < 0) {
            throw new RuntimeException("Conditional throw");
        }
        value = 10;
    }
}

/**
 * Class with a <clinit> that calls a method which might throw.
 * Should NOT be flagged since the throw is not unconditional from clinit's perspective.
 */
class MethodCallClinitClass {
    static int value;
    static {
        value = helperMethod();
    }

    private static int helperMethod() {
        return 42;
    }
}

/**
 * Class that extends a class with normal clinit.
 * Should NOT be flagged.
 */
class ExtendingNormalClass extends NormalClinitClass {
    static String childValue = "child";
}

/**
 * Class with empty static block (does nothing).
 * Should NOT be flagged.
 */
class EmptyStaticBlockClass {
    static {
        // intentionally empty
    }
}

/**
 * Class that calls a method throwing an Error (not Exception).
 * The clinit itself invokes the method, so it doesn't directly throw.
 */
class InvokeErrorThrowingClinitClass {
    static {
        throwError();
    }

    private static void throwError() {
        throw new Error("This method throws an Error");
    }
}
