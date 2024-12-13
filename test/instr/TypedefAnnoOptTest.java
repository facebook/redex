/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


package com.facebook.redex;

import com.facebook.redex.TestStringDef;
import com.facebook.redex.TestIntDef;

public class TypedefAnnoOptTest {

  @TestStringDef public static String testValueOfString() {
    String val = "TWO";
    return TestStringDef.Util.valueOf(val);
  }

  @TestIntDef public static int testValueOfInt() {
    String val = "THREE";
    return TestIntDef.Util.valueOf(val);
  }

}
