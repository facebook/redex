/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexinline;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.Test;

class WithFinalField {
  public final int finalField;
  public WithFinalField(int finalField) {
    this.finalField = finalField;
  }
}

class WithFinalFieldAndFinalize {
  public final int finalField;
  public WithFinalFieldAndFinalize(int finalField) {
    this.finalField = finalField;
  }
  public void finalize() {}
}

class WithNormalField {
  public int normalField;
  public WithNormalField(int normalField) {
    this.normalField = normalField;
  }
}

class WithFinalFieldTwoCtor {
    public final int finalField;
    public WithFinalFieldTwoCtor(int finalField) {
        this.finalField = finalField;
    }
    public WithFinalFieldTwoCtor() {
        this(3);
    }
}

public class MethodInlineRelaxedInitTest {
  @Test
  public void testWithFinalField() {
    WithFinalField a = new WithFinalField(5);
    assertThat(a.finalField).isEqualTo(5);
  }

  @Test
  public void testWithFinalFieldAndFinalize() {
    WithFinalFieldAndFinalize a = new WithFinalFieldAndFinalize(6);
    assertThat(a.finalField).isEqualTo(6);
  }

  @Test
  public void testWithNormalField() {
    WithNormalField a = new WithNormalField(7);
    assertThat(a.normalField).isEqualTo(7);
    a.normalField = 8;
    assertThat(a.normalField).isEqualTo(8);
  }

  @Test
  public void testWithFinalFieldTwoCtor() {
    WithFinalFieldTwoCtor a = new WithFinalFieldTwoCtor();
    assertThat(a.finalField).isEqualTo(3);
  }
}
