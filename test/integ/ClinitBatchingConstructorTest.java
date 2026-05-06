/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

// ============================================================
// Helper classes for constructor safety tests.
// These have NO clinit (or unsafe clinit) and are NOT in the
// method profile, so they are never candidates themselves.
// ============================================================

// Simple helper whose constructor only does IPUT.
class HelperSimpleObj {
  int value;
  HelperSimpleObj(int v) {
    this.value = v;
  }
}

// Base class whose constructor only does IPUT.
class HelperSuperObj {
  int base;
  HelperSuperObj(int b) {
    this.base = b;
  }
}

// Subclass that calls super() then does IPUT.
class HelperSubObj extends HelperSuperObj {
  int extra;
  HelperSubObj(int b, int e) {
    super(b);
    this.extra = e;
  }
}

// Constructor reads a static field via SGET. Has its own clinit.
class HelperWithSgetObj {
  static int sDefault = 10;
  int value;
  HelperWithSgetObj() {
    this.value = sDefault;
  }
}

// Constructor calls a virtual method (toString).
class HelperWithVirtualCallObj {
  String label;
  HelperWithVirtualCallObj(Object o) {
    this.label = o.toString();
  }
}

// Constructor calls a static method (Math.abs).
class HelperWithStaticCallObj {
  int absValue;
  HelperWithStaticCallObj(int v) {
    this.absValue = Math.abs(v);
  }
}

// Safe constructor (IPUT only) but UNSAFE clinit (invoke-static).
class HelperUnsafeClinitObj {
  static String sLabel = String.valueOf(42);
  int value;
  HelperUnsafeClinitObj(int v) {
    this.value = v;
  }
}

// ============================================================
// Test classes for constructor safety. Each has a clinit and
// is added to the method profile so it becomes a candidate.
// ============================================================

// SAFE: clinit instantiates HelperSimpleObj (constructor does IPUT only).
class SafeConstructorClass {
  static HelperSimpleObj s_obj = new HelperSimpleObj(42);
}

// SAFE: clinit instantiates HelperSubObj (super chain, all IPUT).
class SafeConstructorWithSuperClass {
  static HelperSubObj s_obj = new HelperSubObj(10, 20);
}

// SAFE: clinit has multiple new-instance + constant SPUT.
class SafeConstructorMultiFieldClass {
  static HelperSimpleObj s_obj1 = new HelperSimpleObj(1);
  static HelperSimpleObj s_obj2 = new HelperSimpleObj(2);
  static int s_count = 99;
}

// UNSAFE: constructor reads SGET from another class.
class UnsafeConstructorWithSgetClass {
  static HelperWithSgetObj s_obj = new HelperWithSgetObj();
}

// UNSAFE: constructor calls a virtual method.
class UnsafeConstructorWithVirtualCallClass {
  static HelperWithVirtualCallObj s_obj = new HelperWithVirtualCallObj("test");
}

// UNSAFE: constructor calls a static method.
class UnsafeConstructorWithStaticCallClass {
  static HelperWithStaticCallObj s_obj = new HelperWithStaticCallObj(-42);
}

// UNSAFE: instantiated class has an unsafe clinit.
class UnsafeInstantiatedClassClinitClass {
  static HelperUnsafeClinitObj s_obj = new HelperUnsafeClinitObj(42);
}

// SAFE: mixed constants and safe constructor.
class MixedSafeConstAndConstructorClass {
  static int s_count = 100;
  static HelperSimpleObj s_obj = new HelperSimpleObj(42);
  static String s_name = "mixed";
}
