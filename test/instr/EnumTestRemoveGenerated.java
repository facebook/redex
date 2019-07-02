/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
package com.facebook.redextest;

import org.junit.Test;
import static org.fest.assertions.api.Assertions.assertThat;
import java.lang.reflect.Method;
import java.lang.reflect.InvocationTargetException;
import androidx.annotation.Nullable;

enum Season { FALL, WINTER, SPRING, SUMMER }
enum UsesValueOf { ONE, TWO }
enum UsesValuesMethod { ONE, TWO }
enum Captured { ONE, TWO }
enum UsedAsTypeClass { ONE, TWO }
enum Upcasted { ONE, TWO }

 /* Test cases */
public class EnumTestRemoveGenerated {

  private class CapturingClass {
      Season s;
      Object obj;

      // `Season` escapes as original type.
      public void capture_as_self(Season arg) {
        this.s = arg;
      }

      // `Captured` escapes as object type.
      public void capture_as_object(Captured c) {
        this.obj = c;
      }
  };

  // `Season` is upcasted and used as type `Class`, but it does not escape
  // the method.
  private boolean upcast_locally(Season s) {
    switch ((int) (Math.random() * 2)) {
      case 0:
        return (Enum) s == null;
      default:
        return (Object) s == null;
    }
  }

  // Enums that are used as type `Class` could call one of many methods that
  // could lead to reflection, e.g., `Class.getMethods()`.
  private boolean use_as_type_class(UsedAsTypeClass s) {
    Class cl = s.getDeclaringClass();
    return cl == null;
  }

  // `Upcasted` is upcasted and escapes the method.
  public Enum return_type_enum(Upcasted s) {
    return s;
  }

  @Test
  public void generated_methods() {
    CapturingClass capturingClass = new CapturingClass();

    upcast_locally(Season.SPRING);
    capturingClass.capture_as_self(Season.FALL);

    capturingClass.capture_as_object(Captured.ONE);
    use_as_type_class(UsedAsTypeClass.ONE);
    return_type_enum(Upcasted.ONE);

    assertThat(UsesValueOf.valueOf("ONE")).isEqualTo(UsesValueOf.ONE);
    assertThat(UsesValuesMethod.values()).isEqualTo(
      new UsesValuesMethod[]{UsesValuesMethod.ONE, UsesValuesMethod.TWO});

    assertThat(Season.FALL.ordinal()).isEqualTo(0);
    assertThat(UsesValueOf.ONE.ordinal()).isEqualTo(0);
    assertThat(UsesValuesMethod.ONE.ordinal()).isEqualTo(0);
    assertThat(Captured.ONE.ordinal()).isEqualTo(0);
    assertThat(UsedAsTypeClass.ONE.ordinal()).isEqualTo(0);
    assertThat(Upcasted.ONE.ordinal()).isEqualTo(0);
  }
}
