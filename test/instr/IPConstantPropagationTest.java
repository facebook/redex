/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

class TestA {
  public int a;
  public int b;

  public TestA() {
    a = 0;
    b = 1;
  }

  public TestA(int param) { b = param; }
}

class TestB {
  public int a;
  public int b;

  public void change_ifield() {
    double random = Math.random();
    if (random > 1) {
      a = 0;
      b = 1;
    } else {
      a = 0;
      b = 0;
    }
  }
}

class GetItem {
  // CHECK: method: direct redex.GetItem.get_item
  public static int get_item(int input) {
    // CHECK-NOT: const{{.*}} #int 9
    // CHECK: const{{.*}} #int 3
    // CHECK: if-ge {{.*}}
    if (input < 3) {
      // CHECK: const{{.*}} #int 5
      return 5;
    }
    // CHECK: const{{.*}} #int 10
    return 10;
    // CHECK: return {{.*}}
  }
}

class TestC {
  public static int a = GetItem.get_item(2);
  public int another_call() { return GetItem.get_item(9); }
}

class IntegerHolder {
  public static Integer f0 = Integer.valueOf(0);
  public static Integer f1 = Integer.valueOf(1);
  public static Integer f2 = Integer.valueOf(2);
}

class ObjectWithImmutField {
  public final int field;
  public ObjectWithImmutField(int field) {
    this.field = field;
  }
  public int get_field() {
    return this.field;
  }
}

interface TrueVirtualInterface {
  public int calculate(int input);
}

class TrueVirtualInterfaceImpl1 implements TrueVirtualInterface {
  // CHECK: method: virtual redex.TrueVirtualInterfaceImpl1.calculate
  public int calculate(int input) {
    // PRECHECK-NOT: const{{.*}} #int 6
    // POSTCHECK: const{{.*}} #int 6
    return input * 2;
    // CHECK: return {{.*}}
  }
}

class TrueVirtualInterfaceImpl2 implements TrueVirtualInterface {
  // CHECK: method: virtual redex.TrueVirtualInterfaceImpl2.calculate
  public int calculate(int input) {
    // PRECHECK-NOT: const{{.*}} #int 9
    // POSTCHECK: const{{.*}} #int 9
    return input * 3;
    // CHECK: return {{.*}}
  }
}

interface NoChangeNameIntf {
  public int calculate(int input);
}

class NoChangeNameIntfImpl implements NoChangeNameIntf {
  // CHECK: method: virtual redex.NoChangeNameIntfImpl.calculate
  public int calculate(int input) {
    // PRECHECK-NOT: const{{.*}} #int 6
    // POSTCHECK-NOT: const{{.*}} #int 6
    return input * 2;
    // CHECK: return {{.*}}
  }
}

interface AllReturn2Intf {
  public int calculate(int input);
  public int return2();
}

class AllReturn2Impl1 implements AllReturn2Intf {
  public int calculate(int input) { return 5 - input; }
  public int return2() { return 2; }
}

class AllReturn2Impl2 implements AllReturn2Intf {
  public int calculate(int input) { return input - 1; }
  public int return2() { return 2; }
}

interface MayReturn2Intf {
  public int return2();
  public int returnSomething();
}

class MayReturn2Impl1 implements MayReturn2Intf {
  public int return2() { return 2; }
  public int returnSomething() { return 2; }
}

class MayReturn2Impl2 implements MayReturn2Intf {
  public int return2() {
    if (Math.random() >= 0) {
      return 2;
    }
    return 3;
  }
  public int returnSomething() { return 3; }
}

class OverrideWithSameValue {
  public int return3() { return 3; }
}

class OverrideWithSameValue2 extends OverrideWithSameValue {
  @Override
  public int return3() {
    return 3;
  }
}

class OverrideWithDifferentValue {
  public int returnSomething() { return 4; }
}

class OverrideWithDifferentValue2 extends OverrideWithDifferentValue {
  @Override
  public int returnSomething() {
    return 5;
  }
}

public class IPConstantPropagationTest {

  // CHECK: method: virtual redex.IPConstantPropagationTest.two_ctors
  @Test
  public void two_ctors() {
    TestA one = new TestA();
    // PRECHECK: iget {{.*}} redex.TestA.a:int
    // POSTCHECK-NOT: iget {{.*}} redex.TestA.a:int
    assertThat(one.a).isEqualTo(0);
    // CHECK: iget {{.*}} redex.TestA.b:int
    assertThat(one.b).isEqualTo(1);
    TestA two = new TestA(0);
    // PRECHECK: iget {{.*}} redex.TestA.a:int
    // POSTCHECK-NOT: iget {{.*}} redex.TestA.a:int
    assertThat(two.a).isEqualTo(0);
    // CHECK: iget {{.*}} redex.TestA.b:int
    assertThat(two.b).isEqualTo(0);
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.modified_elsewhere
  @Test
  public void modified_elsewhere() {
    TestB one = new TestB();
    one.change_ifield();
    // PRECHECK: iget {{.*}} redex.TestB.a:int
    // POSTCHECK-NOT: iget {{.*}} redex.TestB.a:int
    assertThat(one.a).isEqualTo(0);
    // CHECK: iget {{.*}} redex.TestB.b:int
    assertThat(one.b).isEqualTo(0);
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.call_by_clinit
  @Test
  public void call_by_clinit() {
    TestC c = new TestC();
    assertThat(c.another_call()).isEqualTo(10);
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.use_boxed_integer_constants
  @Test
  public void use_boxed_integer_constants() {
    // PRECHECK: sget-object {{.*}} redex.IntegerHolder.f0:
    // POSTCHECK-NOT: sget-object {{.*}} redex.IntegerHolder.f0:
    Integer f0 = IntegerHolder.f0;
    assertThat(f0.intValue()).isEqualTo(0);
    // PRECHECK: sget-object {{.*}} redex.IntegerHolder.f1:
    // POSTCHECK-NOT: sget-object {{.*}} redex.IntegerHolder.f1:
    Integer f1 = IntegerHolder.f1;
    assertThat(f1.intValue()).isEqualTo(1);
    Integer obj;
    // Can improve.
    if (Math.random() > 0) {
      obj = IntegerHolder.f2;
    } else {
      obj = Integer.valueOf(2);
    }
    assertThat(obj.intValue()).isEqualTo(2);
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.immutable_instance_field
  @Test
  public void immutable_instance_field() {
    ObjectWithImmutField obj = new ObjectWithImmutField(2);
    // PRECHECK: iget {{.*}} redex.ObjectWithImmutField.field
    // POSTCHECK-NOT: iget {{.*}} redex.ObjectWithImmutField.field
    assertThat(obj.field).isEqualTo(2);
    // PRECHECK: invoke-virtual {{.*}} redex.ObjectWithImmutField.get_field
    // Do not support the virtual getter method.
    assertThat(obj.get_field()).isEqualTo(2);

    obj = new ObjectWithImmutField(3);
    // PRECHECK: iget {{.*}} redex.ObjectWithImmutField.field
    // POSTCHECK-NOT: iget {{.*}} redex.ObjectWithImmutField.field
    assertThat(obj.field).isEqualTo(3);
    // PRECHECK: invoke-virtual {{.*}} redex.ObjectWithImmutField.get_field
    assertThat(obj.get_field()).isEqualTo(3);
  }

  @Test
  public void true_virtuals() {
    TrueVirtualInterface a = new TrueVirtualInterfaceImpl1();
    assertThat(a.calculate(3)).isEqualTo(6);
  }

  @Test
  public void true_virtuals_no_rename() {
    NoChangeNameIntf a = new NoChangeNameIntfImpl();
    assertThat(a.calculate(3)).isEqualTo(6);
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.true_virtuals_all_return_2
  @Test
  public void true_virtuals_all_return_2() {
    AllReturn2Intf a;
    if (Math.random() > 0.5) {
      a = new AllReturn2Impl1();
    } else {
      a = new AllReturn2Impl2();
    }
    assertThat(a.calculate(3)).isEqualTo(2);
    assertThat(a.return2()).isEqualTo(2);
    if (a.calculate(3) != 2 || a.return2() != 2) {
      // CHECK-NOT: return-void
      // PRECHECK: invoke-virtual {{.*}} org.fest.assertions.api.BooleanAssert.isTrue
      // POSTCHECK-NOT: invoke-virtual {{.*}} org.fest.assertions.api.BooleanAssert.isTrue
      assertThat(false).isTrue();
    }
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.true_virtuals_may_return_2
  @Test
  public void true_virtuals_may_return_2() {
    MayReturn2Intf a;
    if (Math.random() >= 0) {
      a = new MayReturn2Impl1();
    } else {
      a = new MayReturn2Impl2();
    }
    assertThat(a.return2()).isEqualTo(2);
    assertThat(a.returnSomething()).isEqualTo(2);
    // CHECK-NOT: return-void
    // CHECK: invoke-virtual {{.*}} org.fest.assertions.api.BooleanAssert.isTrue
    if (a.return2() != 2 || a.returnSomething() != 2) {
      assertThat(false).isTrue();
    }
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.true_virtuals_override_same_return
  @Test
  public void true_virtuals_override_same_return() {
    OverrideWithSameValue a = new OverrideWithSameValue();
    assertThat(a.return3()).isEqualTo(3);
    // CHECK-NOT: return-void
    // PRECHECK: invoke-virtual {{.*}} org.fest.assertions.api.BooleanAssert.isTrue
    // POSTCHECK-NOT: invoke-virtual {{.*}} org.fest.assertions.api.BooleanAssert.isTrue
    if (a.return3() != 3) {
      assertThat(false).isTrue();
    }
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.IPConstantPropagationTest.true_virtuals_override_different_return
  @Test
  public void true_virtuals_override_different_return() {
    OverrideWithDifferentValue a = new OverrideWithDifferentValue();
    assertThat(a.returnSomething()).isEqualTo(4);
    // CHECK-NOT: return-void
    // CHECK: invoke-virtual {{.*}} org.fest.assertions.api.BooleanAssert.isTrue
    if (a.returnSomething() != 4) {
      assertThat(false).isTrue();
    }
    // CHECK: return-void
  }
}
