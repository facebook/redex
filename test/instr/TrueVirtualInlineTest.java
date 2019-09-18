/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

interface IA {
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

interface IE {
  public int do_something();
}

abstract class F implements IE {}

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
    // POSTCHECK: invoke-virtual {{.*}} redex.CC.return_self
    assertThat(c.return_self() instanceof CC).isTrue();
    // CHECK: return-void
  }
}
