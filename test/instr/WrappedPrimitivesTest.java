/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import org.junit.Test;
import static org.assertj.core.api.Assertions.assertThat;

public class WrappedPrimitivesTest {
  @Test
  public void testTranformableLookup() {
    long[] results = com.facebook.redex.WrappedPrimitives.run();
    assertThat(results.length).isEqualTo(9);
    assertThat(results[0]).isEqualTo(1);
    assertThat(results[1]).isEqualTo(1);
    assertThat(results[2]).isEqualTo(2);
    assertThat(results[3]).isEqualTo(3);
    assertThat(results[4]).isEqualTo(4);
    assertThat(results[5]).isEqualTo(8);
    assertThat(results[6]).isEqualTo(8);
    assertThat(results[7]).isEqualTo(9);
  }

  @Test
  public void testTranformableCastLookup() {
    long[] results = com.facebook.redex.WrappedPrimitives.runWithInterface();
    assertThat(results.length).isEqualTo(2);
    assertThat(results[0]).isEqualTo(1);
    assertThat(results[1]).isEqualTo(1);
  }
}
