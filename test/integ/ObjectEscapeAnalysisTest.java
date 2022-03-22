/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class ObjectEscapeAnalysisTest {
  public static String Foo;

  static class C {
    int x;
    int y;
    public C(int x, int y) {
      this.x = x;
      Foo = "inlinable side effect";
      this.y = y;
    }
    public int getX() {
      Foo = "another inlinable side effect";
      return this.x;
    }

    public int getY() {
      Foo = "yet another inlinable side effect";
      return this.y;
    }
  }

  static void does_not_leak(Object o) {
  }

  public static int reduceTo42A() {
    C c = new C(23, 19);
    does_not_leak(c);
    return c.getX() + c.getY();
  }


  static class D {
    int x;
    public D(int x) {
      this.x = x;
    }
    public static D allocator(int x) {
      return new D(x);
    }
    public int getX() {
      return this.x;
    }
  }

  public static int reduceTo42B() {
    D d = D.allocator(42);
    return d.getX();
  }


  static class E {
    int x;
    public E(int x) {
      this.x = x;
    }
    public int getX() {
      return this.x;
    }
  }
  static class F {
    int x;
    public F(int x) {
      this.x = x;
    }
    public int getX() {
      return this.x;
    }
  }

  public static int reduceTo42C() {
    E e = new E(21);
    F f = new F(21);
    return e.getX() + f.getX();
  }


  static class G {
    static Object leak;
    public G() {
      leak = this;
    }
    public G(H h) {
    }
    public int getX() {
      return 42;
    }
  }

  public static int doNotReduceTo42A() {
    G g = new G();
    return g.getX();
  }


  static class H {
    public H() {
    }
    public int getX() {
      return 42;
    }
  }

  public static int doNotReduceTo42B() {
    H h = new H();
    G g = new G(h);
    return h.getX();
  }


  static class I {
    int x;
    public I(int x) {
      this.x = x;
    }
    public static I allocator(int x) {
      return new I(x);
    }
    public int getX() {
      return this.x;
    }
  }

  public static boolean reduceTo42IdentityMatters() {
    I i = I.allocator(42);
    return i == null;
  }


  static class J {
    public J() {}
  }

  static class DontOptimizeFinalInInit {
    final int x;
    int y;
    DontOptimizeFinalInInit() {
      J j = new J();
      int x = read_x(j);
      this.x = 42;
      this.y = this.x; // must not use previously read value of x (even though x is final)
    }
    int read_x(J j) {
      return this.x;
    }
  }


  static class K {
    int x;
    static int X;
    static class CyclicStaticInitDependency {
      static int Y;
      static { Y = X; }
    }
    static {
      // Static initialization order matters.
      X = CyclicStaticInitDependency.Y;
    }
    public K(int x) {
      this.x = x;
    }
    public int getX() {
      return this.x;
    }
  }

  public static int reduceTo42WithInitClass() {
    K k = new K(42);
    return k.getX();
  }


  static class L {
    int x;
    public L(int x) {
      this.x = x;
    }
    public synchronized int getX() {
      return this.x;
    }
  }

  public static int reduceTo42WithMonitors() {
    L l = new L(42);
    synchronized (l) {
      return l.getX();
    }
  }


  static class M {
    int x;
    public M(int x) {
      this.x = x;
    }
    public void add(int other) {
      this.x += this.x * other;
      this.x += this.x * other;
      this.x += this.x * other;
    }
    public int get() {
      return this.x;
    }
  }

  public static int reduceTo42WithMultiples1(int x) {
    M m = new M(x);
    m.add(1);
    m.add(2);
    m.add(3);
    return m.get();
  }

  public static int reduceTo42WithMultiples2(int x) {
    M m = new M(x);
    m.add(1);
    m.add(2);
    m.add(3);
    return m.get();
  }


  public static class N {
    int x;
    public N(Builder builder) {
      this.x = builder.x;
    }
    public static class Builder {
      public int x;
      public Builder(int x) {
        this.x = x;
      }
    }
    public int get() {
      return this.x;
    }
  }

  public static N reduceTo42WithExpandedCtor() {
    return new N(new N.Builder(42));
  }
}
