/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

class DedupBlocksTest {

    public void someFunc() {
      System.out.println("some");
    }

    public void otherFunc() {
      System.out.println("other");
    }

    public int useSwitch() {
      switch ((int) (Math.random() * 10)) {
      case 0:
        someFunc();
        break;
      case 1:
        someFunc();
        break;
      case 2:
        someFunc();
        break;
      default:
        otherFunc();
        break;
      }
      return 0;
    }
}
