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
import java.io.Serializable;

enum UsesValueOf {
  ONE, TWO;
  public int otherField; /* prevent replace_enum_with_int */
}
enum UsesValuesMethod {
  ONE, TWO;
  public int otherField;
}
enum UsesNothing {
  ONE, TWO;
  public int otherField;
}
enum Captured {
  ONE, TWO;
  public int otherField;
}
enum UsedAsTypeClass {
  ONE, TWO;
  public int otherField;
}
enum Upcasted {
  ONE, TWO;
  public int otherField;
}
enum UpcastedToSerializable {
  ONE, TWO;
  public int otherField;
}

 /* Test cases */
public class EnumTestRemoveGenerated {

  private class CapturingClass {
      UsesValuesMethod s;
      Object obj;

      // `UsesValuesMethod` escapes as original type.
      public void capture_as_self(UsesValuesMethod arg) {
        this.s = arg;
      }

      // `Captured` escapes as object type.
      public void capture_as_object(Captured c) {
        this.obj = c;
      }
  };

  // `UsesValues` is upcasted and used as type `Class`, but it does not escape
  // the method.
  private boolean upcast_locally(UsesValuesMethod s) {
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

  private boolean upcast_to_serializable(Serializable s) {
    return s == null;
  }

  @Test
  public void generated_methods() {
    CapturingClass capturingClass = new CapturingClass();
    capturingClass.capture_as_self(UsesValuesMethod.ONE);
    capturingClass.capture_as_object(Captured.ONE);
    upcast_locally(UsesValuesMethod.ONE);
    use_as_type_class(UsedAsTypeClass.ONE);
    return_type_enum(Upcasted.ONE);
    upcast_to_serializable(UpcastedToSerializable.ONE);

    UsesValueOf.valueOf("ONE");
    UsesValuesMethod.values();

    int a = UsesValueOf.ONE.ordinal();
    a = UsesValuesMethod.ONE.ordinal();
    a = UsesNothing.ONE.ordinal();
    a = Captured.TWO.ordinal();
    a = UsedAsTypeClass.ONE.ordinal();
    a = Upcasted.ONE.ordinal();
    a = UpcastedToSerializable.TWO.ordinal();
    a = UsesValuesMethod.ONE.compareTo(UsesValuesMethod.TWO);
    a = UsesValuesMethod.TWO.hashCode();
    boolean b = UsesValuesMethod.ONE.equals(UsesValuesMethod.TWO);
    String s = UsesValuesMethod.TWO.name();
    s = UsesValuesMethod.TWO.toString();
  }
}
