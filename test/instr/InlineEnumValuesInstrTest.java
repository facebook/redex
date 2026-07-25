/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.Test;

/*
 * javac 15+ (JDK-8241798) compiles the $VALUES array through a synthetic
 * `private static E[] $values()` method that <clinit> invokes. This test feeds
 * that real javac output into Redex with OptimizeEnumsPass.inline_enum_values
 * enabled and verifies the pre/post shape directly on the bytecode:
 *
 *  - PRE  (real javac input): the synthetic $values() method exists and <clinit>
 *    invokes it.
 *  - POST (inlined):          $values() is gone and its array-build now lives
 *    inline in <clinit> (new-array + sput-object $VALUES), i.e. the pre-JDK-15
 *    layout is restored.
 *
 * max_enum_size=0 in the config disables replace_enum_with_int so the enum
 * survives as an enum and the inlined <clinit> is observable in the output.
 */

// CHECK-LABEL: class: redex.Inlinable
// PRECHECK: method: direct redex.Inlinable.$values:()redex.Inlinable[]
// POSTCHECK-NOT: method: direct redex.Inlinable.$values:()redex.Inlinable[]
enum Inlinable {
  ALPHA,
  BETA,
  GAMMA,
  DELTA;
}

public class InlineEnumValuesInstrTest {

  // PRE: <clinit> delegates the array build to the synthetic $values().
  // PRECHECK-LABEL: method: direct redex.Inlinable.<clinit>:()void
  // PRECHECK: invoke-static {{.*}} redex.Inlinable.$values:()redex.Inlinable[]

  // POST: the array build has been inlined back into <clinit>, which now
  // constructs the array itself and stores it into $VALUES with no helper call.
  // POSTCHECK-LABEL: method: direct redex.Inlinable.<clinit>:()void
  // POSTCHECK: new-array
  // POSTCHECK: sput-object {{.*}} redex.Inlinable.$VALUES:redex.Inlinable[]
  // POSTCHECK-NOT: invoke-static {{.*}} redex.Inlinable.$values
  // POST: and the synthetic $values() method itself is deleted from the class.
  // POSTCHECK-NOT: method: direct redex.Inlinable.$values:()redex.Inlinable[]

  @Test
  public void valuesRoundTrips() {
    Inlinable[] vs = Inlinable.values();
    assertThat(vs.length).isEqualTo(4);
    assertThat(vs[0]).isEqualTo(Inlinable.ALPHA);
    assertThat(vs[3]).isEqualTo(Inlinable.DELTA);
  }

  @Test
  public void valueOfRoundTrips() {
    assertThat(Inlinable.valueOf("BETA")).isEqualTo(Inlinable.BETA);
    assertThat(Inlinable.GAMMA.ordinal()).isEqualTo(2);
  }
}
