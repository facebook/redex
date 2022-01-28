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
}
