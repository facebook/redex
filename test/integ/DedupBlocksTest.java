/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
