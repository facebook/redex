/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import org.junit.Test;
import static org.fest.assertions.api.Assertions.assertThat;
import java.io.Serializable;
import java.util.EventObject;

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

  // Enums that are used as type `Class` could call one of many methods that
  // could lead to reflection, e.g., `Class.getMethods()`.
  boolean use_as_type_class(UsedAsTypeClass s) {
    Class cl = s.getDeclaringClass();
    return cl == null;
  }

  // `Upcasted` is upcasted and escapes the method.
  Enum return_type_enum(Upcasted s) {
    return s;
  }

  boolean upcast_to_serializable(Serializable s) {
    return s == null;
  }

  @Test
  public void main() {
    CapturingClass capturingClass = new CapturingClass();
    capturingClass.capture_as_self(UsesValuesMethod.ONE);
    capturingClass.capture_as_object(Captured.ONE);
    upcast_locally(UsesValuesMethod.ONE);
    use_as_type_class(UsedAsTypeClass.ONE);
    return_type_enum(Upcasted.ONE);
    upcast_to_serializable(UpcastedToSerializable.ONE);

    UsesValueOf.valueOf("ONE");
    UsesValuesMethod.values();

    assertThat(UsesValueOf.ONE.ordinal()).isEqualTo(0);
    assertThat(UsesValuesMethod.ONE.ordinal()).isEqualTo(0);
    assertThat(UsesNothing.ONE.ordinal()).isEqualTo(0);
    assertThat(Captured.TWO.ordinal()).isEqualTo(1);
    assertThat(UsedAsTypeClass.ONE.ordinal()).isEqualTo(0);
    assertThat(Upcasted.ONE.ordinal()).isEqualTo(0);
    assertThat(UpcastedToSerializable.TWO.ordinal()).isEqualTo(1);
    UsesValuesMethod.ONE.compareTo(UsesValuesMethod.TWO);
    UsesValuesMethod.TWO.hashCode();
    assertThat(UsesValuesMethod.ONE.equals(UsesValuesMethod.TWO)).isFalse();
    assertThat(UsesValuesMethod.TWO.name()).isEqualTo("TWO");
    assertThat(UsesValuesMethod.TWO.toString()).isEqualTo("TWO");
    assertThat(AsInstanceField.ONE.toString()).isEqualTo("ONE");
  }
}
