/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

class ImmutableSubcomponentAnalyzer {
  static class E {}

  static class D {
    public D(E e) {
      m_e = e;
    }
    public E getE() { return m_e; }

    private E m_e;
  }

  static class C {
    public C(E e) {
      m_e = e;
    }
    public E getE() { return m_e; }

    private E m_e;
  }

  static class B {
    public B(D d) {
      m_d = d;
    }
    public D getD() { return m_d; }
    public E makeE() { return new E(); }

    private D m_d;
  }

  static class A {
    public A(B b, C c) {
      m_b = b;
      m_c = c;
    }
    public B getB() { return m_b; }
    public C getC() { return m_c; }

    private B m_b;
    private C m_c;
  }

  static class Structure {
    public Structure(A a, B b) {
      m_a = a;
      m_b = b;
    }
    public A getA() { return m_a; }
    public B getB() { return m_b; }

    private A m_a;
    private B m_b;
  }

  static void check(A a, B b, C c, D d, E e) {}

  static void test(Structure s1, Structure s2) {
    A a1 = s1.getA();
    B b1 = a1.getB();
    E e1 = b1.getD().getE();

    B b2 = s2.getB();
    E e2 = b2.getD().getE();
    D d2 = b2.getD();
    E e3 = s2.getA().getB().makeE();

    Structure s = new Structure(a1, b1);
    A a2 = s.getA();
    B b3 = a2.getB();
    C c = s.getA().getC();
    D d3 = s.getB().getD();

    check(a1, b1, s1.getA().getC(), s1.getB().getD(), e1);
    check(s2.getA(), b2, s2.getA().getC(), d2, e2);
    check(a2, b3, c, d3, e3);
  }
}
