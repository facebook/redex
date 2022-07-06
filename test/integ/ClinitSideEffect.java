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

class ClinitOnlyTouchOwnFields {
  static String s_str = "";
  static int s_a = 0;
}

class ExtendNoSideEffect extends ClinitOnlyTouchOwnFields {
  static double s_d = 0.0;
}

@HasSideffects
class ClinitHasInvoke {
  static {
    System.out.println("ClinitHasInvoke.<clinit>");
  }
}

@HasSideffects
class ExtendSideEffect extends ClinitHasInvoke {
}

// Can improve
@HasSideffects
class ClinitTouchOtherFields {
  static String s_str = ClinitOnlyTouchOwnFields.s_str;
}
