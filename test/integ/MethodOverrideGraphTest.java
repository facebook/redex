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

class C extends A implements IB {
  public void m() {}
  // A's definition of `n` will implement IB::n.
}

// Miranda test fixtures: ID declares p(); AbstractD implements ID but does
// not concretely implement p(). Subclasses ConcreteD1/ConcreteD2 supply p().
// With include_miranda=true, the graph synthesizes a miranda slot at
// AbstractD that bridges ID.p -> AbstractD.miranda(p).
interface ID {
  public void p();
}

abstract class AbstractD implements ID {
  // Note: no concrete p() -- miranda case.
}

class ConcreteD1 extends AbstractD {
  public void p() {}
}

class ConcreteD2 extends AbstractD {
  public void p() {}
}
