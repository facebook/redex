/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

interface IA {
  // PRECHECK: method: virtual redex.IA.do_something
  // POSTCHECK: method: virtual redex.IA.do_something
  public int do_something();
}

interface IE {
  // PRECHECK: method: virtual redex.IE.do_something
  // POSTCHECK-NOT: method: virtual redex.IE.do_something
  public int do_something();
}

abstract class BB implements IA {
  public BB return_self() { return this; }
}

class CC extends BB {
  // PRECHECK: method: virtual redex.CC.do_something
  // POSTCHECK: method: virtual redex.CC.do_something
  public int do_something() { return 1; }

  @Override
  public BB return_self() {
    return this;
  }
}

class D extends BB {
  // PRECHECK: method: virtual redex.D.do_something
  // POSTCHECK: method: virtual redex.D.do_something
  public int do_something() { return 2; }
}

abstract class F implements IE {
  // PRECHECK: method: virtual redex.F.do_something
  // POSTCHECK-NOT: method: virtual redex.F.do_something
  public abstract int do_something();
}

class G extends F {
  // PRECHECK: method: virtual redex.G.do_something
  // POSTCHECK-NOT: method: virtual redex.G.do_something
  public int do_something() { return 3; }
}

class H extends F {
  // PRECHECK: method: virtual redex.H.do_something
  // POSTCHECK-NOT: method: virtual redex.H.do_something
  public int do_something() { return 4; }
}

interface J {
  int getInt();
}
interface K {
  int getAnotherInt();
}

interface IGetInt {
  public int getInt();
  public int getAnotherInt();
  public J getJ();
  public int add();
}

abstract class GetInt implements IGetInt {}

class GetInt1 extends GetInt implements J, K {
  // CHECK: method: virtual redex.GetInt1.getAnotherInt
  public int getAnotherInt() { return 2; }

  // PRECHECK: method: virtual redex.GetInt1.getInt
  // POSTCHECK-NOT: method: virtual redex.GetInt1.getInt
  public int getInt() { return 1; }

  // PRECHECK: method: virtual redex.GetInt1.getJ
  // POSTCHECK-NOT: method: virtual redex.GetInt1.getJ
  public J getJ() { return this; }

  // PRECHECK: method: virtual redex.GetInt1.add
  // POSTCHECK: method: virtual redex.GetInt1.add
  public int add() { J j = this; K k = this; return j.getInt() + k.getAnotherInt(); }
}

class GetInt2 extends GetInt implements J, K {
  // CHECK: method: virtual redex.GetInt2.getAnotherInt
  public int getAnotherInt() { return 3; }

  // PRECHECK: method: virtual redex.GetInt2.getInt
  // POSTCHECK-NOT: method: virtual redex.GetInt2.getInt
  public int getInt() { return 1; }

  // PRECHECK: method: virtual redex.GetInt2.getJ
  // POSTCHECK-NOT: method: virtual redex.GetInt2.getJ
  public J getJ() { return this; }

  // PRECHECK: method: virtual redex.GetInt2.add
  // POSTCHECK: method: virtual redex.GetInt2.add
  public int add() { J j = this; K k = this; return j.getInt() + k.getAnotherInt(); }
}

class GetInt3 extends GetInt implements J, K {
  // CHECK: method: virtual redex.GetInt3.getAnotherInt
  public int getAnotherInt() { return 4; }

  // PRECHECK: method: virtual redex.GetInt3.getInt
  // POSTCHECK-NOT: method: virtual redex.GetInt3.getInt
  public int getInt() { return 1; }

  // PRECHECK: method: virtual redex.GetInt3.getJ
  // POSTCHECK-NOT: method: virtual redex.GetInt3.getJ
  public J getJ() { return this; }

  // PRECHECK: method: virtual redex.GetInt3.add
  // POSTCHECK: method: virtual redex.GetInt3.add
  public int add() { J j = this; K k = this; return j.getInt() + k.getAnotherInt(); }
}

class SameImplementation {
  // PRECHECK: method: virtual redex.SameImplementation.getInt
  // POSTCHECK-NOT: method: virtual redex.SameImplementation.getInt
  public int getInt() { return 1; }
}

class SameImplementation2 extends SameImplementation {
  // PRECHECK: method: virtual redex.SameImplementation2.getInt
  // POSTCHECK-NOT: method: virtual redex.SameImplementation2.getInt
  @Override
  public int getInt() {
    return 1;
  }
}

abstract class AAA {
  public abstract int return_BBB_field();
  public abstract BBB return_BBB_self();
}

class BBB extends AAA {
  int field = 42;

  @Override
  public int return_BBB_field() {
    return field;
  }

  @Override
  public BBB return_BBB_self() {
    return this;
  }
}

public class TrueVirtualInlineTest {

  // CHECK: method: virtual redex.TrueVirtualInlineTest.test_do_something
  @Test
  public void test_do_something() {
    CC c = new CC();
    // PRECHECK: invoke-virtual {{.*}} redex.CC.do_something
    // POSTCHECK-NOT: invoke-virtual {{.*}} redex.CC.do_something
    assertThat(c.do_something()).isEqualTo(1);

    H h = new H();
    // PRECHECK: invoke-virtual {{.*}} redex.H.do_something
    // POSTCHECK-NOT: invoke-virtual {{.*}} redex.H.do_something
    assertThat(h.do_something()).isEqualTo(4);

    BB b;
    if (Math.random() > 1) {
      b = new CC();
    } else {
      b = new D();
    }

    // PRECHECK: invoke-virtual {{.*}} redex.BB.do_something
    // POSTCHECK: invoke-virtual {{.*}} redex.BB.do_something
    assertThat(b.do_something()).isEqualTo(2);

    IA a = new CC();
    // PRECHECK: invoke-interface {{.*}} redex.IA.do_something
    // POSTCHECK: invoke-interface {{.*}} redex.IA.do_something
    assertThat(a.do_something()).isEqualTo(1);
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.TrueVirtualInlineTest.test_return_self
  @Test
  public void test_return_self() {
    CC c = new CC();
    // PRECHECK: invoke-virtual {{.*}} redex.CC.return_self
    // POSTCHECK-NOT: invoke-virtual {{.*}} redex.CC.return_self
    assertThat(c.return_self() instanceof CC).isTrue();
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.TrueVirtualInlineTest.test_return_BBB_field
  @Test
  public void test_return_BBB_field() {
    AAA a = new BBB();
    // PRECHECK: invoke-virtual {{.*}} redex.AAA.return_BBB_field
    // POSTCHECK: check-cast {{.*}} redex.BBB
    assertThat(a.return_BBB_field() == 42).isTrue();
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.TrueVirtualInlineTest.test_return_BBB_self
  @Test
  public void test_return_BBB_self() {
    AAA a = new BBB();
    // PRECHECK: invoke-virtual {{.*}} redex.AAA.return_BBB_self
    // POSTCHECK: check-cast {{.*}} redex.BBB
    assertThat(a.return_BBB_self() instanceof BBB).isTrue();
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.TrueVirtualInlineTest.test_same_implementation
  @Test
  public void test_same_implementation() {
    GetInt get_int;
    if (Math.random() > 1) {
      get_int = new GetInt1();
    } else if (Math.random() < 0) {
      get_int = new GetInt3();
    } else {
      // get_int should be of type GetInt2
      get_int = new GetInt2();
    }

    // PRECHECK: invoke-virtual {{.*}} redex.GetInt.getInt
    // POSTCHECK-NOT: invoke-virtual {{.*}} redex.GetInt.getInt
    assertThat(get_int.getInt()).isEqualTo(1);

    // PRECHECK: invoke-virtual {{.*}} redex.GetInt.getAnotherInt
    // POSTCHECK: invoke-virtual {{.*}} redex.GetInt.getAnotherInt
    assertThat(get_int.getAnotherInt()).isEqualTo(3);
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.TrueVirtualInlineTest.test_same_implementation
  @Test
  public void test_same_implementation2() {
    SameImplementation get_int;
    if (Math.random() > 0.5) {
      get_int = new SameImplementation();
    } else {
      get_int = new SameImplementation2();
    }

    // PRECHECK: invoke-virtual {{.*}} redex.SameImplementation.getInt
    // POSTCHECK-NOT: invoke-virtual {{.*}} redex.SameImplementation.getInt
    assertThat(get_int.getInt()).isEqualTo(1);
    // CHECK: return-void
  }

  // CHECK: method: virtual redex.TrueVirtualInlineTest.test_same_implementation3
  @Test
  public void test_same_implementation3() {
    GetInt get_int;
    if (Math.random() > 1) {
      get_int = new GetInt1();
    } else if (Math.random() < 0) {
      get_int = new GetInt3();
    } else {
      get_int = new GetInt2();
    }

    // PRECHECK: invoke-virtual {{.*}} redex.GetInt.getJ
    // POSTCHECK: check-cast {{.*}} redex.J
    assertThat(get_int.getJ() instanceof GetInt).isTrue();
  }

  // CHECK: method: virtual redex.TrueVirtualInlineTest.test_same_implementation4
  @Test
  public void test_same_implementation4() {
    GetInt get_int;
    if (Math.random() > 1) {
      get_int = new GetInt1();
    } else if (Math.random() < 0) {
      get_int = new GetInt3();
    } else {
      get_int = new GetInt2();
    }

    // PRECHECK: invoke-virtual {{.*}} redex.GetInt.add
    // POSTCHECK: invoke-virtual {{.*}} redex.GetInt.add
    assertThat(get_int.add() > 0).isTrue();
  }
}
