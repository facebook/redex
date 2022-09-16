/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

interface Intf {
  public int get();
}

class Impl implements Intf{
  public int get() { return 42; }
}

class Helper {
// The following methods will be injected by the test. The reason
// is that javac doesn't compile the following code but it's type
// safe at bytecode level.
//
//   Object createIntf() {
//     return new Impl();
//   }

//   Intf retIntf() {
//     return createIntf();
//   }
}
