/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

public class Omega {

  public class Alpha {
    void red() {}
    void green0() {}
    void green1() {}
    void blue() {}
  }

  public class Beta {
    void green2() {}
    void blue() {}
  }

  public class Gamma {
    void red() {}
    void blue() {}
  }
}
