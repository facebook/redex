/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import javax.security.auth.x500.X500Principal;

class Foo {
  private int x;

  // One removable arg
  public Foo(int x) {
    this.x = 4;
  }

  // No removable args
  public Foo(int x, int y) {
    this.x = x + y;
  }

  // One colliding removable arg
  public Foo(int x, int y, double z) {
    this.x = x + y + 3;
  }
}

class FooVirtual {
  private int x;

  // One removable arg
  public FooVirtual(int x) {
    this.x = 4;
  }

  // No removable args
  public FooVirtual(int x, int y) {
    this.x = x + y;
  }

  public void used_in_base(int x) {
    this.x = x;
  }

  public void used_in_derived(int x) {
    this.x = 3;
  }

  public void used_in_all(int x) {
    this.x = x;
  }

  public void used_in_none(int x) {
    this.x = 2;
  }

  public void used_in_derived_derived(int x) {
    this.x = 3;
  }

  public void used_in_sibling(int x) {
    this.x = 3;
  }
}

interface FooInterface {
  public void used_in_all(int x);
  public void used_in_none(int x);
  public void used_in_base(int x);
}

class FooDerived extends FooVirtual implements FooInterface {
  private int x;

  // One removable arg
  public FooDerived(int x) {
    super(x);
  }

  // No removable args
  public FooDerived(int x, int y) {
    super(x, y);
  }

  public void used_in_base(int x) {
    this.x = 3;
  }

  public void used_in_sibling(int x) {
    this.x = 3;
  }

  public void used_in_derived(int x) {
    this.x = x;
  }

  public void used_in_all(int x) {
    this.x = x;
  }

  public void used_in_none(int x) {
    this.x = 1;
  }
}

class FooDerivedDerived extends FooDerived {
  private int x;

  // One removable arg
  public FooDerivedDerived(int x) {
    super(x);
  }

  public void used_in_base(int x) {
    this.x = 3;
  }

  public void used_in_derived_derived(int x) {
    this.x = x;
  }

  public void used_in_all(int x) {
    this.x = x;
  }

  public void used_in_none(int x) {
    this.x = 1;
  }
}

class FooSibling extends FooVirtual {
  private int x;

  // One removable arg
  public FooSibling(int x) {
    super(x);
  }

  public void used_in_sibling(int x) {
    this.x = x;
  }

  public void used_in_all(int x) {
    this.x = x;
  }

  public void used_in_none(int x) {
    this.x = 1;
  }
}

abstract class FooAbstract {
  private int x;

  public abstract void abstract_method_used(int x);

  public abstract void abstract_method_unused(int x);
}

class FooAbstractDerived extends FooAbstract {
  private int x;

  public void abstract_method_used(int x) {
    this.x = x;
  }

  public void abstract_method_unused(int x) {
    this.x = 3;
  }
}

class FooUser {
  private int x;

  public void use_foo1() {
    Foo foo = new Foo(x);
  }

  public void use_foo2() {
    Foo foo = new Foo(1, 1);
  }

  public void use_foo3() {
    double d = 5.5;
    Foo foo = new Foo(1, 1, d);
  }
}

class FooVirtualUser {
  private int x;

  public void use_foo1_virtual() {
    FooVirtual foo = new FooVirtual(1);
  }

  public void use_foo1_derived() {
    FooDerived foo = new FooDerived(1);
  }

  public void use_foo2_virtual() {
    FooVirtual foo = new FooVirtual(1, 2);
  }

  public void use_foo2_derived() {
    FooDerived foo = new FooDerived(1, 2);
  }

  public void use_foo_used_in_base_virtual() {
    FooVirtual foo = new FooVirtual(1);
    foo.used_in_base(1);
  }

  public void use_foo_used_in_base_derived() {
    FooDerived foo = new FooDerived(1);
    foo.used_in_base(1);
  }

  public void use_foo_used_in_derived_virtual() {
    FooVirtual foo = new FooVirtual(1);
    foo.used_in_derived(1);
  }

  public void use_foo_used_in_derived_derived() {
    FooDerived foo = new FooDerived(1);
    foo.used_in_derived(1);
  }

  public void use_foo_used_in_all_virtual() {
    FooVirtual foo = new FooVirtual(1);
    foo.used_in_all(1);
  }

  public void use_foo_used_in_all_derived() {
    FooDerived foo = new FooDerived(1);
    foo.used_in_all(1);
  }

  public void use_foo_used_in_none_virtual() {
    FooVirtual foo = new FooVirtual(1);
    foo.used_in_none(1);
  }

  public void use_foo_used_in_none_derived() {
    FooDerived foo = new FooDerived(1);
    foo.used_in_none(1);
  }

  public void use_foo_used_in_base_derived_derived() {
    FooDerivedDerived foo = new FooDerivedDerived(1);
    foo.used_in_base(1);
  }

  public void use_foo_used_in_derived_derived_virtual() {
    FooVirtual foo = new FooVirtual(1);
    foo.used_in_derived_derived(1);
  }

  public void use_foo_used_in_sibling_derived() {
    FooDerived foo = new FooDerived(1);
    foo.used_in_sibling(1);
  }

  public void use_foo_used_in_none_interface() {
    FooInterface foo = new FooDerived(1);
    foo.used_in_none(1);
  }

  public void use_foo_used_in_all_interface() {
    FooInterface foo = new FooDerived(1);
    foo.used_in_all(1);
  }

  public void use_foo_used_in_base_interface() {
    FooInterface foo = new FooDerived(1);
    foo.used_in_base(1);
  }

  public void use_foo_abstract_used() {
    FooAbstract foo = new FooAbstractDerived();
    foo.abstract_method_used(1);
  }

  public void use_foo_abstract_unused() {
    FooAbstract foo = new FooAbstractDerived();
    foo.abstract_method_unused(1);
  }
}

class Statics {
  private static String greeting;
  private static double wide;
  public static void static1() {
    String hello = "I don't have arguments!";
  }

  public static void static2(boolean truthy) {
    String bonjour = "J'ai un argument";
    if (truthy) {
      bonjour = "Je use the argument";
    }
    Statics.greeting = bonjour;
  }

  public static void static3(Object obj, double wide_param) {
    String ciao = "Je ne sais pas Italian; two arguments";
    wide_param *= 2.0;
    ciao = "wide_param is alive, obj is dead";
    Statics.wide = wide_param;
  }

  public static int static4_with_result() {
    return 0;
  }

  public static int static5_with_result() {
    return 0;
  }
}

class StaticsUser {
  public void use_static1() {
    Statics.static1();
  }

  public void use_static2() {
    Statics.static2(true);
  }

  public void use_static3(Foo foo) {
    double dub = 4.0;
    Statics.static3(foo, dub);
  }

  public void use_static4_with_result(Foo foo) {
    int x = Statics.static4_with_result();
    x = x / x;
  }
}

class Privates {
  private double save;
  public void use_private_first() {
    private1(1.0, 2.0);
  }

  public void use_private_second() {
    private1(0, 1.0, 2.0);
  }

  // first
  private void private1(double x, double y) {
    this.save = x + y;
  }

  // second
  private void private1(int c, double x, double y) {
    this.save = x * y;
  }
}

class Parent {
}

class NonVirtuals extends Parent {
  public void non_virtual1(int x) {
    x = 5;
  }

  protected void non_virtual2(double x) {
    x = 5.0;
  }
}

class NonVirtualsUser {
  public void use_non_virtual1(NonVirtuals nv) {
    nv.non_virtual1(1);
  }

  public void use_non_virtual2(NonVirtuals nv) {
    nv.non_virtual2(1.0);
  }
}

class Reorderables {
  int int_value;
  Object obj_value;
  double double_value;

  public void reorderable1(int x, Object y, double z) {
    this.int_value = x;
    this.obj_value = y;
    this.double_value = z;
  }

  public void reorderable2(double x, int y, Object z) {
    this.double_value = x;
    this.int_value = y;
    this.obj_value = z;
  }

  public void reorderable2(Object x, double y, int z) {
    this.obj_value = x;
    this.double_value = y;
    this.int_value = z;
  }

  public void reorderable2(Object x, int y, double z) {
    this.obj_value = x;
    this.int_value = y;
    this.double_value = z;
  }
}

interface ReorderablesInterface {
  void reorderable1(int x, Object y, double z);
  void reorderable2(double x, int y, Object z);
  void reorderable2(Object x, double y, int z);
  void reorderable2(Object x, int y, double z);
}

class SubReorderables extends Reorderables implements ReorderablesInterface {
  public void reorderable1(int x, Object y, double z) {
  }

  public void reorderable2(double x, int y, Object z) {
  }

  public void reorderable2(Object x, double y, int z) {
  }

  public void reorderable2(Object x, int y, double z) {
  }
}
