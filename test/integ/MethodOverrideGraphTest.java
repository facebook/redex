/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

interface IA {
  public void m();
}

interface IB extends IA {
  public void m();
  public void n();
}

interface IC {
  public void m();
}

class A implements IA {
  public void m() {}
  public void n() {}
}

class B extends A implements IB, IC {
  public void m() {}
  // A's definition of `n` will implement IB::n.
}
