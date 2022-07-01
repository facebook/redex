/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

class WithInit {
  // CHECK: method: direct redex.WithInit.<init>:()void
  public WithInit() {}
  public WithInit(int i) {}
}

public class DelinitWithReflectionTest {
  @Test
  public void check_method() {
    try {
      Object with_init = Class.forName("redex.WithInit").newInstance();
      assertThat(true).isEqualTo(true);
    } catch (Exception ex) {
      // We should not have exception
      assertThat(false).isEqualTo(true);
    }
  }
}
