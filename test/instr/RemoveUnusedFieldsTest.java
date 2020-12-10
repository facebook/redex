/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

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
  // Removal of Strings are excluded in the test Redex config
  // CHECK: redex.RemoveUnusedFieldsTest.unusedString:java.lang.String
  String unusedString;

  @RuntimeAnno(
    getterVisibility = RuntimeAnno.Visibility.NONE
  )
  public void init() {
    unusedInt = 1;
    unusedString = "foo";
  }
}
