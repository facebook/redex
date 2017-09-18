/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
