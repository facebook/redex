/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.proguard;

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

public class Alpha {

    private int wombat;
    public int numbat;
    public String omega;
    public List<String> theta;

    // static final nulls have to be at the end of static fields we write out
    private static final int anum = 5;
    public static final Object brand = new Object();

    public Alpha () {
        wombat = 18;
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
