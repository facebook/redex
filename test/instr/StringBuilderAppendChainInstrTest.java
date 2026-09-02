/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.Test;

public class StringBuilderAppendChainInstrTest {

  // `a + b + x`: the two constant appends merge into a single append of "ab".
  // CHECK-LABEL: method: virtual redex.StringBuilderAppendChainInstrTest.concatConstantsThenValue
  String concatConstantsThenValue(String x) {
    String a = "a";
    String b = "b";
    // PRECHECK: const-string {{.*}} "a"
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK: const-string {{.*}} "b"
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK-NOT: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    // POSTCHECK: new-instance {{.*}} java.lang.StringBuilder
    // POSTCHECK: const-string {{.*}} "ab"
    // POSTCHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // POSTCHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // POSTCHECK-NOT: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // POSTCHECK: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    return a + b + x;
  }

  // `a + b`: the whole string is known, so the toString() becomes a const-string and the builder
  // disappears.
  // CHECK-LABEL: method: virtual redex.StringBuilderAppendChainInstrTest.concatConstantsOnly
  String concatConstantsOnly() {
    String a = "a";
    String b = "b";
    // PRECHECK: new-instance {{.*}} java.lang.StringBuilder
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    // POSTCHECK-NOT: java.lang.StringBuilder
    // POSTCHECK: const-string {{.*}} "ab"
    // POSTCHECK-NOT: java.lang.StringBuilder
    // POSTCHECK: return-object
    return a + b;
  }

  // `a + x + b`: the two constants are not adjacent, so nothing merges.
  // CHECK-LABEL: method: virtual redex.StringBuilderAppendChainInstrTest.concatValueBetweenConstants
  String concatValueBetweenConstants(String x) {
    String a = "a";
    String b = "b";
    // CHECK: const-string {{.*}} "a"
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK: const-string {{.*}} "b"
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK-NOT: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // CHECK: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    return a + x + b;
  }

  // `x + y`: two appends of runtime values become String.concat.
  // CHECK-LABEL: method: direct redex.StringBuilderAppendChainInstrTest.concatTwoValues
  private String concatTwoValues(String x, String y) {
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.append
    // PRECHECK-NOT: invoke-virtual {{.*}} java.lang.String.concat
    // PRECHECK: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    // POSTCHECK: invoke-virtual {{.*}} java.lang.String.concat
    // POSTCHECK-NOT: invoke-virtual {{.*}} java.lang.StringBuilder.toString
    // POSTCHECK: return-object
    return x + y;
  }

  @Test
  public void twoValuesConcatenateInOrder() {
    assertThat(concatTwoValues("a", "b")).isEqualTo("ab");
  }

  @Test
  public void constantsThenValueConcatenateInOrder() {
    assertThat(concatConstantsThenValue("c")).isEqualTo("abc");
  }

  @Test
  public void constantsOnlyProduceTheirConcatenation() {
    assertThat(concatConstantsOnly()).isEqualTo("ab");
  }

  @Test
  public void valueBetweenConstantsConcatenatesInOrder() {
    assertThat(concatValueBetweenConstants("x")).isEqualTo("axb");
  }
}
