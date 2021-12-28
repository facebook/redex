/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.lang.annotation.Target;
import java.lang.annotation.ElementType;

@Target(ElementType.TYPE)
@interface HasSideffects {
}

// Benign clinits

class ClinitOnlyTouchOwnFields {
  static String s_str = "";
  static int s_a = 0;
}

class ExtendNoSideEffect extends ClinitOnlyTouchOwnFields {
  static double s_d = 0.0;
}

class ClinitHasBenignInvoke {
  static int s_field = 0;
  static int s_benign = Math.max(42, 108);
}

class ExtendClinitHasBenignInvoke extends ClinitHasBenignInvoke {
}

class ReadBenign {
  static String s_str = ClinitOnlyTouchOwnFields.s_str;
  static int s_field = ClinitHasBenignInvoke.s_field;
}

// Non-Benign clinits

@HasSideffects
class ClinitHasNonBenignInvoke {
  static int s_field = 0;
  static {
    System.loadLibrary("boo");
  }
}

@HasSideffects
class ExtendClinitHasNonBenignInvoke extends ClinitHasNonBenignInvoke {
}

@HasSideffects
class ReadNonBenign {
  static int s_field = ClinitHasNonBenignInvoke.s_field;
}

// Non-Benign clinits because of cyclic dependency

@HasSideffects
class CycleA {
  static int s_field = CycleB.s_field + 23;
}

@HasSideffects
class CycleB {
  static int s_field = CycleA.s_field + 42;
}

// Non-Benign clinits because of puts

@HasSideffects
class WritesToOtherClass {
  static {
    OtherClass.s_field = 42;
  }
}

class OtherClass {
  static int s_field;
}

// Benign constructor calls

class BenignConstructorCalls {
  BenignConstructorCalls() {}
  static BenignConstructorCalls theInstance = new BenignConstructorCalls();
}

class BenignConstructorCalls2 extends BenignConstructorCalls {
  int x;
  String s;
  int z;
  BenignConstructorCalls nested;
  BenignConstructorCalls2(int x, String s) {
    this.x = x;
    this.s = s;
    this.z = ClinitHasBenignInvoke.s_benign;
    this.nested = new BenignConstructorCalls();
    }
  static BenignConstructorCalls2 theInstance = new BenignConstructorCalls2(42, "Hello");
}

// Benign method calls

class BenignMethodCalls {
  static int s_field;
  static void helper() {
    s_field = 42;
  }
  static {
    helper();
  }
}

// Non-Benign constructor + method calls

// TODO: We do better by distinguishing clinits with external side-effects
// (e.g. calling loadLibrary) from those that merely have internal side-effects
// (e.g. cyclic depndencies).

@HasSideffects
class NonBenignConstructorCalls {
  int field_a;
  int field_b;
  NonBenignConstructorCalls() {
    field_a = CycleA.s_field;
    field_b = ClinitHasNonBenignInvoke.s_field;
  }
  static NonBenignConstructorCalls theInstance = new NonBenignConstructorCalls();
}

@HasSideffects
class NonBenignMethodCalls {
  static int s_field_a;
  static int s_field_b;
  static void helper() {
    s_field_a = CycleA.s_field;
    s_field_b = ClinitHasNonBenignInvoke.s_field;
  }
  static {
    helper();
  }
}
