/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


package com.facebook.redex;

import android.annotation.SuppressLint;
import com.facebook.redex.annotations.SafeIntDef;
import java.lang.annotation.ElementType;
import java.lang.annotation.Target;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import com.facebook.common.dextricks.StringTreeSet;

@Retention(RetentionPolicy.RUNTIME)
@SafeIntDef
@Target({ElementType.METHOD, ElementType.FIELD, ElementType.PARAMETER, ElementType.LOCAL_VARIABLE, ElementType.TYPE_USE})
public @interface TestIntDef {
  int ONE = 1;
  int TWO = 2;
  int THREE = 3;
  int FOUR = 4;

  public class Util {
    @TestIntDef public static int valueOf(String val) {
      if (val == null) {
        throw new NullPointerException("the value passed into valueOf cannot be null");
      }
      switch (val) {
        case "ONE":
          return 1;
        case "TWO":
          return 2;
        case "THREE":
          return 3;
        case "FOUR":
          return 4;
        default:
        throw new IllegalArgumentException("invalid argument " + val + " does not exist within the typedef");
      }
    }

    @SuppressLint("WrongIntDefValue")
    @TestIntDef 
    private static int valueOfOpt(String val) {
      if (val == null) {
        throw new NullPointerException("the value passed into valueOf cannot be null");
      }
      String map_encoding = "";
      int output = StringTreeSet.searchMap(val, map_encoding, Integer.MIN_VALUE);
      if (output == Integer.MIN_VALUE) {
        throw new IllegalArgumentException("invalid argument " + val + " does not exist within the typedef");
      }
      return output;
    }
  }
}
