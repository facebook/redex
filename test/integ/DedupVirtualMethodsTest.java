/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.lang.annotation.Target;
import java.lang.annotation.ElementType;

/**
 * All the methods that are annotated by Duplication will be removed.
 */
@Target(ElementType.METHOD)
@interface Duplication {
}

/**
 * All the methods that are annotated by Duplication will be publicized.
 */
@Target(ElementType.METHOD)
@interface Publicized {
}

class P {
  public int method() {
    return 0;
  }
  @Publicized
  protected int non_public_method() {
    return 1;
  }
}

class C1 extends P {}
class C11 extends C1 {
  @Duplication
  public int method() {
    return 0;
  }
  @Duplication
  public int non_public_method() {
    return 1;
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
  // This is not deduplicated but its access flag should be public after the transformation.
  @Publicized
  protected int non_public_method() {
    return 2;
  }
}
class C21 extends C2 {
  @Duplication
  public int method() {
    return 0;
  }
}

class PD {
  public int method() {
    return 0;
  }
}

class C2D extends PD {
  @Duplication
  public int method() {
    return 0;
  }

  void callV() {
    C21D c = new C21D();
    c.method();
  }
}
class C21D extends C2D {
  @Duplication
  public int method() {
    return 0;
  }
}
