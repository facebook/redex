/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import integ.TestIntDef;
import integ.TestStringDef;

@interface NotSafeAnno {}

interface I {
  int get();
}

public class TypedefAnnosTest {
  // test that TypeInference can correctly parse through all the parameter annotations
  static @NotSafeAnno @TestIntDef int testIntAnnoParam(@NotSafeAnno @TestIntDef int val) {
    return val;
  }

  static @NotSafeAnno @TestStringDef String testStringAnnoParam(@NotSafeAnno @TestStringDef String val) {
    return val;
  }

  static @TestIntDef int testAnnoInt(@TestIntDef int o) {
    if (o > TestIntDef.TWO) {
      return o;
    }
    return TestIntDef.FOUR;
  }

  static @TestStringDef String testAnnoString(@TestStringDef String o) {
    if (o.equals(TestStringDef.THREE)) {
      return o;
    }
    return TestStringDef.ONE;
  }

  static @TestIntDef I testAnnoObject(@TestIntDef I o) {
    return o;
  }

  // test OPCODE_INVOKE_STATIC on different argument types
  static @TestIntDef I testAnnoInvokeStatic(@TestIntDef I o) {
    return testAnnoObject(o);
  }

  static @TestIntDef int testIntAnnoInvokeStatic(@TestIntDef int o) {
    return testAnnoInt(o);
  }

  static @TestStringDef String testStringAnnoInvokeStatic(@TestStringDef String o) {
    return testAnnoString(o);
  }
}
