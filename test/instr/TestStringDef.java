/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


package com.facebook.redex;

import android.annotation.SuppressLint;
import com.facebook.redex.annotations.SafeStringDef;
import java.lang.annotation.ElementType;
import java.lang.annotation.Target;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import com.facebook.common.dextricks.StringTreeSet;

@Retention(RetentionPolicy.RUNTIME)
@SafeStringDef
@Target({ElementType.METHOD, ElementType.FIELD, ElementType.PARAMETER, ElementType.LOCAL_VARIABLE, ElementType.TYPE_USE})
public @interface TestStringDef {
  String ONE = "one";
  String TWO = "two";
  String THREE = "three";
  String FOUR = "four";

  public class Util {
    @TestStringDef public static String valueOf(String val) {
      if (val == null) {
        throw new NullPointerException("the value passed into valueOf cannot be null");
      }
      switch (val) {
        case "ONE":
          return "one";
        case "TWO":
          return "two";
        case "THREE":
          return "three";
        case "FOUR":
          return "four";
        default:
        throw new IllegalArgumentException("invalid argument " + val + " does not exist within the typedef");
      }
    }
    @SuppressLint("WrongStringDefValue")
    @TestStringDef 
    private static String valueOfOpt(String val) {
      if (val == null) {
        throw new NullPointerException("the value passed into valueOf cannot be null");
      }
      String map_encoding = "";
      String output = StringTreeSet.searchStringToStringMap(val, map_encoding, "");
      if (output.isEmpty()) {
        throw new IllegalArgumentException("invalid argument " + val + " does not exist within the typedef");
      }
      return output;
    }
  }
}
