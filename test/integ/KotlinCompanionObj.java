/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest.objtestjava;

public final class KotlinCompanionObj {
  public static final KotlinCompanionObj.Companion Companion = new KotlinCompanionObj.Companion();

  public final String get() {
    return "test1";
  }

  public static final class Companion {
    public final String get() {
      return "test2";
    }
    private Companion() {
    }
  }
}

class FooJava {
  public static void main() {
    KotlinCompanionObj cls = new KotlinCompanionObj();
    System.out.print (cls.get());
    System.out.print (cls.Companion.get());
  }
}
