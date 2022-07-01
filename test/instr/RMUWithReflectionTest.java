/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

class WithTwoMethods {
  // PRECHECK: method: virtual redex.WithTwoMethods.do_something:(float, float)void
  // POSTCHECK-NOT: method: virtual redex.WithTwoMethods.do_something:(float, float)void
  public void do_something(float i, float j) {}
  // CHECK: method: virtual redex.WithTwoMethods.do_something:(float, int)void
  public void do_something(float i, int j) {}
}

public class RMUWithReflectionTest {
  @Test
  public void check_method() {
    try {
      Class<?> clazz = Class.forName("redex.WithTwoMethods");
      assertThat(clazz.getMethod("do_something", float.class, int.class))
          .isNotNull();
    } catch (Exception ex) {
      // nothing
    }
  }
}
