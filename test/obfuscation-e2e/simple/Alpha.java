/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

import java.util.concurrent.atomic.AtomicIntegerFieldUpdater;
import java.util.concurrent.atomic.AtomicLongFieldUpdater;
import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;
import java.util.List;

interface IntfParent {
  public double get();
  public void abstract_impl();
}

interface IntfSub extends IntfParent {
  public int concrete_method();
}

abstract class AbstractClass implements IntfSub {
  public abstract void abstract_method();
  public int concrete_method() { return 1; }
  public void abstract_impl() { }
}

interface Intf1 {
  public void intf_meth();
}

interface Intf2 extends Intf1 {
  public void intf_meth2();
}

class Foo extends AbstractClass implements Intf2 {
  public void not_overridden() {  }
  private void priv_meth() {}
  public void intf_meth() { }
  public void intf_meth2() { }
  @Override
  public void abstract_method() { }
  @Override
  public double get() { return 5.0; }
  public static Foo get_foo() { return new Foo(); }
}

class Bar implements Intf1 {
  public void random_vmeth() {}
  public void intf_meth() { }
}

class Baz {
  public Baz() {
    IntfSub foo = Foo.get_foo();
    foo.abstract_impl();
    IntfParent abc = new AbstractClass() {
      @Override
      public double get() { return 0.0; }
      @Override
      public void abstract_method() {} };
    abc.get();
  }
}

interface ReflectedInterface {
    public void reflectedI1();
    public void reflectedI2();
    public void reflectedI3();
    public void unreflectedI4();
}

public class Alpha implements ReflectedInterface {

    private int wombat;
    public int numbat;
    public String omega;
    public List<String> theta;
    public int reflected1;
    public int reflected2;
    public volatile int reflected3;
    public volatile long reflected4;
    public volatile Object reflected5;
    public int reflected6;

    // static final nulls have to be at the end of static fields we write out
    private static final int anum = 5;
    public static final Object brand = new Object();

    public static void reflected1() {}
    public static void reflected2() {}
    public static void reflected3() {}

    public void reflected4() {}
    public void reflected5() {}
    public void reflected6() {}

    public void reflectedI1() {}
    public void reflectedI2() {}
    public void reflectedI3() {}
    public void unreflectedI4() {}

    public Alpha () throws Exception {
        wombat = 18;
        Alpha.class.getField("reflected1");
        Alpha.class.getDeclaredField("reflected2");
        Alpha.class.getMethod("reflected1");
        Alpha.class.getDeclaredMethod("reflected2");
        Alpha.class.getDeclaredMethod("reflected3", new Class[] {});
        Alpha.class.getMethod("reflected4");
        Alpha.class.getDeclaredMethod("reflected5");
        Alpha.class.getDeclaredMethod("reflected6", new Class[] {});
        ReflectedInterface.class.getMethod("reflectedI1");
        ReflectedInterface.class.getDeclaredMethod("reflectedI2");
        ReflectedInterface.class.getDeclaredMethod("reflectedI3", new Class[] {});
        AtomicIntegerFieldUpdater.newUpdater(Alpha.class, "reflected3");
        AtomicLongFieldUpdater.newUpdater(Alpha.class, "reflected4");
        AtomicReferenceFieldUpdater.newUpdater(Alpha.class, Object.class, "reflected5");
    }

    public Alpha (int v) {
        wombat = v;
    }

    public static int someDmethod() { return 5; }

    public static void anotherDmethod(int x) { }

    private int privateDmethod() { return 8; }

    public int doubleWombat() {
        privateDmethod();
        return 2 * wombat;
    }

   public int doubleWombat(int x) {
       return 2 * wombat * x;
   }

   public int tripleWombat() {
       return 3 * wombat + anum;
   }
}
