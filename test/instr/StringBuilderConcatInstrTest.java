/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.Test;

/**
 * `a + b` is what produces the two-append builder the reduction targets, so these methods are
 * written the way the pattern actually arises rather than as hand-built StringBuilder chains; the
 * PRECHECK directives pin down that shape. The reduction only retargets toString() to String.concat
 * -- the builder it orphans is removed by the ObjectSensitiveDcePass this config runs after it, so
 * the allocation really does go away.
 */
public class StringBuilderConcatInstrTest {

  // A null guard leaves both operands non-null on the branch the concatenation
  // is on, which is the nullness the reduction requires.
  // CHECK-LABEL: method: virtual redex.StringBuilderConcatInstrTest.guardedOperands
  String guardedOperands(String a, String b) {
    if (a == null || b == null) {
      return "";
    }
    // PRECHECK: new-instance {{.*}} java.lang.StringBuilder
    // PRECHECK: invoke-direct {{.*}} java.lang.StringBuilder.<init>
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    // POSTCHECK-NOT: new-instance {{.*}} java.lang.StringBuilder
    // POSTCHECK-NOT: invoke-direct {{.*}} java.lang.StringBuilder.<init>
    // POSTCHECK-NOT: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // POSTCHECK-NOT: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    // POSTCHECK: invoke-virtual {{.*}} java.lang.String.concat
    return a + b;
  }

  // String.concat throws on a null argument where append writes the text
  // "null", so an operand that is not provably non-null keeps its builder.
  // CHECK-LABEL: method: virtual redex.StringBuilderConcatInstrTest.nullableOperand
  String nullableOperand(String a, String b) {
    // CHECK: new-instance {{.*}} java.lang.StringBuilder
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK-NOT: java.lang.String.concat
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    return a + b;
  }

  // Three appends are not transformed. The operands are guarded so the count is
  // the only reason this one keeps its builder.
  // CHECK-LABEL: method: virtual redex.StringBuilderConcatInstrTest.threeGuardedOperands
  String threeGuardedOperands(String a, String b, String c) {
    if (a == null || b == null || c == null) {
      return "";
    }
    // CHECK: new-instance {{.*}} java.lang.StringBuilder
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK-NOT: java.lang.String.concat
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    return a + b + c;
  }

  // CHECK-LABEL: method: virtual redex.StringBuilderConcatInstrTest.guardedOperandsConcatenate
  @Test
  public void guardedOperandsConcatenate() {
    assertThat(guardedOperands("foo", "bar")).isEqualTo("foobar");
  }

  @Test
  public void threeOperandsConcatenate() {
    assertThat(threeGuardedOperands("foo", "bar", "baz")).isEqualTo("foobarbaz");
  }

  @Test
  public void nullOperandKeepsNullText() {
    assertThat(nullableOperand("foo", null)).isEqualTo("foonull");
  }
}
