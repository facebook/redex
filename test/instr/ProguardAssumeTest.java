/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import android.annotation.SuppressLint;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.Test;

@SuppressLint("JavatestsStaticField")
public class ProguardAssumeTest {
  private int assumeNoSideEffectsField = 0;

  private int assumeNoSideEffects() {
    assumeNoSideEffectsField++;  // Actual side effect.
    assertThat(assumeNoSideEffectsField).isEqualTo(1);  // But should not be executed twice.
    return assumeNoSideEffectsField;
  }

  @Test
  public void testAssumeNoSideEffectsStripped() {
    int first = assumeNoSideEffects();
    assumeNoSideEffects();  // This should be optimized away.
    assertThat(first).isEqualTo(1);
    assertThat(assumeNoSideEffectsField).isEqualTo(1);
  }

  private static boolean assumeNoSideEffectsField2 = true;

  private static boolean assumeNoSideEffectsAssumeValue() {
    assumeNoSideEffectsField2 = !assumeNoSideEffectsField2;  // Actual side effect.
    assertThat(assumeNoSideEffectsField2).isEqualTo(false);  // But should not be executed twice.
    return assumeNoSideEffectsField2;
  }

  @Test
  public void testAssumeNoSideEffectsMethodValueStripped() {
    boolean val = assumeNoSideEffectsAssumeValue();
    assertThat(val).isEqualTo(false);  // Should be assumed return value, not actual value.
    assertThat(assumeNoSideEffectsField2).isEqualTo(true);  // Should not be changed.
  }

  // Something that should be true.
  private final static boolean assumeNoSideEffectsField3 = Math.random() >= 0;

  @Test
  public void testAssumeNoSideEffectsFieldValue() {
    assertThat(assumeNoSideEffectsField3).isEqualTo(false);  // Return value.
  }

  private static boolean assumeValueField = false;

  private static boolean assumeValue() {
    assumeValueField = true;
    return assumeValueField;
  }

  @Test
  public void testAssumeValueMethod() {
    assertThat(assumeValueField).isEqualTo(false);
    boolean val = assumeValue();
    assertThat(val).isEqualTo(false);
    assertThat(assumeValueField).isEqualTo(true);
  }

  private static boolean assumeValueField2 = false;

  private static boolean assumeValue2() {
    assumeValueField2 = true;
    return assumeValueField2;
  }

  @Test
  public void testAssumeValueMethodNotStripped() {
    assertThat(assumeValueField2).isEqualTo(false);
    assumeValue2();
    assertThat(assumeValueField2).isEqualTo(true);
  }

  // Something that should be true.
  private final static boolean assumeValueField3 = Math.random() >= 0;

  @Test
  public void testAssumeValueFieldValue() {
    assertThat(assumeValueField3).isEqualTo(false);  // Return value.
  }
}
