/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package integ;

import com.facebook.redex.annotations.SafeIntDef;
import java.lang.annotation.ElementType;
import java.lang.annotation.Target;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

// An IntDef with {0, 2, 3} — notably missing 1.
// Used to test error paths when boolean/XOR values (which produce 0 or 1)
// are used with an IntDef that doesn't contain both 0 and 1.
@Retention(RetentionPolicy.RUNTIME)
@SafeIntDef
@Target({ElementType.METHOD, ElementType.FIELD, ElementType.PARAMETER, ElementType.LOCAL_VARIABLE, ElementType.TYPE_USE})
public @interface TestIntDefNoBool {
    int ZERO = 0;
    int TWO = 2;
    int THREE = 3;
}
