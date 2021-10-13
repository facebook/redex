/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

class A {
    public static void foo() {
        System.out.println("foo");
    }
}

class B {
    public static void bar() {
        System.out.println("bar");
        A.foo();
    }
}

public class VerifierTest {
    public void main() {
        B.bar();
    }
}
