/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package integ;

import com.facebook.redex.annotations.SafeStringDef;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.RUNTIME)
@SafeStringDef
public @interface TestStringDef {
    String ONE = "one";
    String TWO = "two";
    String THREE = "three";
    String FOUR = "four";
}
