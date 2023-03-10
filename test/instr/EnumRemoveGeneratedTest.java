/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import org.junit.Test;
import static org.assertj.core.api.Assertions.assertThat;
import java.io.Serializable;
import java.util.EventObject;

/**
 * The existence of the non-primitive/non-String field on the following enums prevents the replace_enum_with_int optimization.
 * So they will be opitmized to non-enum classes.
 * The focus of this test is on generated util methods like values and valueOf are handled properly by the optimization.
 */

// CHECK-LABEL: class: redex.UsesValueOf
// CHECK: Superclass: java.lang.Enum
enum UsesValueOf {
  ONE, TWO;
  // CHECK: method: direct redex.UsesValueOf.valueOf:(java.lang.String)redex.UsesValueOf
  // CHECK: method: direct redex.UsesValueOf.values:()redex.UsesValueOf[]
  Integer otherField; /* prevent replace_enum_with_int */
}

// CHECK-LABEL: class: redex.UsesValuesMethod
// CHECK: Superclass: java.lang.Enum
enum UsesValuesMethod {
  ONE, TWO;
  // CHECK-NOT: method: direct redex.UsesValuesMethod.valueOf:(java.lang.String)redex.UsesValuesMethod
  // CHECK: method: direct redex.UsesValuesMethod.values:()redex.UsesValuesMethod[]
  Integer otherField;
}

// CHECK-LABEL: class: redex.UsesNothing
// CHECK: Superclass: java.lang.Enum
enum UsesNothing {
  ONE, TWO;
  // CHECK-NOT: method: direct redex.UsesNothing.valueOf:(java.lang.String)redex.UsesNothing
  // CHECK-NOT: method: direct redex.UsesNothing.values:()redex.UsesNothing[]
  Integer otherField;
}

// CHECK-LABEL: class: redex.Captured
// CHECK: Superclass: java.lang.Enum
enum Captured {
  ONE, TWO;
  // CHECK: method: direct redex.Captured.valueOf:(java.lang.String)redex.Captured
  // CHECK: method: direct redex.Captured.values:()redex.Captured[]
  Integer otherField;
}

// CHECK-LABEL: class: redex.UsedAsTypeClass
// CHECK: Superclass: java.lang.Enum
enum UsedAsTypeClass {
  ONE, TWO;
  // CHECK: method: direct redex.UsedAsTypeClass.valueOf:(java.lang.String)redex.UsedAsTypeClass
  // CHECK: method: direct redex.UsedAsTypeClass.values:()redex.UsedAsTypeClass[]
  Integer otherField;
}

// CHECK-LABEL: class: redex.Upcasted
// CHECK: Superclass: java.lang.Enum
enum Upcasted {
  ONE, TWO;
  // CHECK: method: direct redex.Upcasted.valueOf:(java.lang.String)redex.Upcasted
  // CHECK: method: direct redex.Upcasted.values:()redex.Upcasted[]
  Integer otherField;
}

// CHECK-LABEL: class: redex.UpcastedToSerializable
// CHECK: Superclass: java.lang.Enum
enum UpcastedToSerializable {
  ONE, TWO;
  // CHECK: method: direct redex.UpcastedToSerializable.valueOf:(java.lang.String)redex.UpcastedToSerializable
  // CHECK: method: direct redex.UpcastedToSerializable.values:()redex.UpcastedToSerializable[]
  Integer otherField;
}

// CHECK-LABEL: class: redex.AsInstanceField
// CHECK: Superclass: java.lang.Enum
enum AsInstanceField {
  ONE, TWO;
  // CHECK: method: direct redex.AsInstanceField.valueOf:(java.lang.String)redex.AsInstanceField
  // CHECK: method: direct redex.AsInstanceField.values:()redex.AsInstanceField[]
  Integer otherField;
}

public class EnumRemoveGeneratedTest {
  // EventObject implements Serializable.
  class ImplementsSerializable extends EventObject {
    AsInstanceField field;
    ImplementsSerializable(Object source) { super(source); }
  }

  @Test
  public void testAsInstanceField() {
    assertThat(AsInstanceField.ONE.toString()).isEqualTo("ONE");
  }

  static class CapturingClass {
      static UsesValuesMethod s;
      static UsesNothing field;
      Object obj;

      // `UsesValuesMethod` escapes as original type.
      public void capture_as_self(UsesValuesMethod arg) {
        this.s = arg;
      }

      // `Captured` escapes as object type.
      public void capture_as_object(Captured c) {
        this.obj = c;
      }
  }

  @Test
  public void testEscapeToFields() {
    CapturingClass capturingClass = new CapturingClass();
    capturingClass.capture_as_self(UsesValuesMethod.ONE);
    capturingClass.capture_as_object(Captured.ONE);
    assertThat(Captured.TWO.ordinal()).isEqualTo(1);
    assertThat(UsesNothing.ONE.ordinal()).isEqualTo(0);
  }

  // `UsesValues` is upcasted and used as type `Class`, but it does not escape
  // the method.
  boolean upcast_locally(UsesValuesMethod s) {
    switch ((int) (Math.random() * 2)) {
      case 0:
        return (Enum) s == null;
      default:
        return (Object) s == null;
    }
  }

  @Test
  public void testUpcastLocally() {
    upcast_locally(UsesValuesMethod.ONE);
  }

  // Enums that are used as type `Class` could call one of many methods that
  // could lead to reflection, e.g., `Class.getMethods()`.
  boolean use_as_type_class(UsedAsTypeClass s) {
    Class cl = s.getDeclaringClass();
    return cl == null;
  }

  @Test
  public void testGetDeclaringClass() {
    use_as_type_class(UsedAsTypeClass.ONE);
    assertThat(UsedAsTypeClass.ONE.ordinal()).isEqualTo(0);
  }

  // `Upcasted` is upcasted and escapes the method.
  Enum return_type_enum(Upcasted s) {
    return s;
  }

  @Test
  public void testEscapeAsReturnType() {
    return_type_enum(Upcasted.ONE);
    assertThat(Upcasted.ONE.ordinal()).isEqualTo(0);
  }

  boolean upcast_to_serializable(Serializable s) {
    return s == null;
  }

  @Test
  public void testUpcastToSerializable() {
    upcast_to_serializable(UpcastedToSerializable.ONE);
    assertThat(UpcastedToSerializable.TWO.ordinal()).isEqualTo(1);
  }

  @Test
  public void testUseOfValueOf() {
    UsesValueOf.valueOf("ONE");
    assertThat(UsesValueOf.ONE.ordinal()).isEqualTo(0);
  }

  @Test
  public void testUseOfValues() {
    UsesValuesMethod.values();
    assertThat(UsesValuesMethod.ONE.ordinal()).isEqualTo(0);
    UsesValuesMethod.ONE.compareTo(UsesValuesMethod.TWO);
    UsesValuesMethod.TWO.hashCode();
    assertThat(UsesValuesMethod.ONE.equals(UsesValuesMethod.TWO)).isFalse();
    assertThat(UsesValuesMethod.TWO.name()).isEqualTo("TWO");
    assertThat(UsesValuesMethod.TWO.toString()).isEqualTo("TWO");
  }
}
