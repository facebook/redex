/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class MySpecialException extends Exception {
  void foo() {}
}

class MySpecialException1 extends MySpecialException {}
class MySpecialException2 extends MySpecialException {}

public class TypeInferenceTest {
  static void whatIsThisThrowable(Throwable t) {}

  static void mayThrowSpecialException() throws MySpecialException {
    throw new MySpecialException();
  }

  static void mayThrowEither1or2() throws MySpecialException1, MySpecialException2 {}

  static void testExceptionTypeInference() {
    try {
      mayThrowSpecialException();
    } catch (MySpecialException e) {
      whatIsThisThrowable(e);
    }
  }

  static void testCatch2Types() {
    try {
      mayThrowEither1or2();
    } catch (MySpecialException1 | MySpecialException2 e) {
      e.foo();
      whatIsThisThrowable(e); // Expect e to be of type MySpecialException
    }
  }
}
