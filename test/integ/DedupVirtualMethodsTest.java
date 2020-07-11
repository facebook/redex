/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.lang.annotation.Target;
import java.lang.annotation.ElementType;

@Target(ElementType.METHOD)
@interface Duplication {
}

class P {
  public int method() {
    return 0;
  }
}

class C1 extends P {}
class C11 extends C1 {
  @Duplication
  public int method() {
    return 0;
  }
}
class C111 extends C11 {
  public int method() {
    return 1;
  }
}
class C1111 extends C111 {
  public int method() {
    return 0;
  }
}

class C2 extends P {
  @Duplication
  public int method() {
    return 0;
  }
}
class C21 extends C2 {
  @Duplication
  public int method() {
    return 0;
  }
}
