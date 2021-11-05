/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.ref.WeakReference;

// CHECK-LABEL: class: redex.RuntimeAnno
@Retention(RetentionPolicy.RUNTIME)
@interface RuntimeAnno {
  // CHECK-LABEL: class: redex.RuntimeAnno$Visibility
  public enum Visibility {
    // The enum field NONE is referenced by latter annotation, so the construction code is not deleted.
    // CHECK: sput-object {{.*}} redex.RuntimeAnno$Visibility.NONE
    // PRECHECK: sput-object {{.*}} redex.RuntimeAnno$Visibility.PUBLIC_ONLY
    // POSTCHECK-NOT: sput-object {{.*}} redex.RuntimeAnno$Visibility.PUBLIC_ONLY
    NONE, DEFAULT, PUBLIC_ONLY;
  }

  Visibility getterVisibility() default Visibility.DEFAULT;
}

// CHECK-LABEL: class: redex.RemoveUnusedFieldsTest
class RemoveUnusedFieldsTest {
  // NOTE: Be careful with `CHECK-NOT`. A typo can yield a false positive.
  // CHECK-NOT: redex.RemoveUnusedFieldsTest.unusedInt:int
  int unusedInt;
  // CHECK-NOT: redex.RemoveUnusedFieldsTest.unusedBox:java.lang.Integer
  Integer unusedBox;
  // Removal of Strings are excluded in the test Redex config
  // CHECK: redex.RemoveUnusedFieldsTest.unusedString:java.lang.String
  String unusedString;
  // CHECK: redex.RemoveUnusedFieldsTest.unusedEscapingObject:redex.SomeObject
  SomeObject unusedEscapingObject;
  // CHECK: redex.RemoveUnusedFieldsTest.unusedCapturingObject:redex.SomeObject
  SomeObject unusedCapturingObject;
  // CHECK: redex.RemoveUnusedFieldsTest.unusedDirectlyCapturingObject:redex.SomeObject
  SomeObject unusedDirectlyCapturingObject;
  // CHECK: redex.RemoveUnusedFieldsTest.unusedCapturingSubObject:redex.SomeSubObject
  SomeSubObject unusedCapturingSubObject;
  // CHECK-NOT: redex.RemoveUnusedFieldsTest.unusedNonEscapingObject:redex.SomeObject
  SomeObject unusedNonEscapingObject;
  // CHECK: redex.RemoveUnusedFieldsTest.unusedEscapingArray:java.lang.Object[]
  Object[] unusedEscapingArray;
  // CHECK: redex.RemoveUnusedFieldsTest.unusedCapturingArray:java.lang.Object[]
  Object[] unusedCapturingArray;
  // CHECK-NOT: redex.RemoveUnusedFieldsTest.unusedNonEscapingArray:java.lang.Object[]
  Object[] unusedNonEscapingArray;
  // CHECK-NOT: redex.RemoveUnusedFieldsTest.unusedTwiceWrittenField1:java.lang.Object[]
  Object[] unusedTwiceWrittenField1;
  // CHECK-NOT: redex.RemoveUnusedFieldsTest.unusedTwiceWrittenField2:java.lang.Object[]
  Object[] unusedTwiceWrittenField2;

  @RuntimeAnno(
    getterVisibility = RuntimeAnno.Visibility.NONE
  )

  public void init() {
    unusedInt = 1;
    unusedString = "foo";
    unusedBox = 1;
    SomeObject someObject = new SomeObject();
    System.out.println(someObject); // escapes here
    unusedEscapingObject = someObject;
    unusedCapturingObject = new SomeObject(this);
    someObject = new SomeObject();
    someObject.object = this;
    unusedDirectlyCapturingObject = someObject;
    unusedCapturingSubObject = new SomeSubObject(this);
    unusedNonEscapingObject = new SomeObject();
    Object[] someArray = new Object[1];
    System.out.println(someArray); // escapes here
    unusedEscapingArray = someArray;
    unusedCapturingArray = new Object[]{this};
    unusedNonEscapingArray = new Object[1];
    someArray = new Object[1];
    unusedTwiceWrittenField1 = someArray;
    unusedTwiceWrittenField2 = someArray;
    UsingSomeClassWithClinitWithSideEffect.init();
    UsingSomeClassWithoutClinit.init();
  }
}

// CHECK-LABEL: class: redex.SomeObject
class SomeObject {
  public Object object;
  Integer harmless;
  public SomeObject() {
    this.harmless = new Integer(1);
  }
  public SomeObject(Object object) {
    this.object = object;
  }
}

// CHECK-LABEL: class: redex.SomeSubObject
class SomeSubObject extends SomeObject {
  public SomeSubObject() {
  }
  public SomeSubObject(Object object) {
    super(object);
  }
}

// CHECK-LABEL: class: redex.UsingSomeClassWithClinitWithSideEffect
class UsingSomeClassWithClinitWithSideEffect {
  public static void init() {
    // CHECK: redex.SomeClassWithClinitWithSideEffect.$redex_init_class:redex.SomeClassWithClinitWithSideEffect
    SomeClassWithClinitWithSideEffect.unusedField = 1;
  }
}

// CHECK-LABEL: class: redex.SomeClassWithClinitWithSideEffect
class SomeClassWithClinitWithSideEffect {
  static {
    try {
      System.loadLibrary("boo"); // side effect
    } catch (Throwable t) {
    }
  }
  public static int unusedField;
}

// CHECK-LABEL: class: redex.UsingSomeClassWithoutClinit
class UsingSomeClassWithoutClinit {
  public static void init() {
    // CHECK-NOT: redex.SomeClassWithoutClinit.$redex_init_class:redex.SomeClassWithoutClinit
    SomeClassWithoutClinit.unusedField = 1;
  }
}

class SomeClassWithoutClinit {
  public static int unusedField;
}
