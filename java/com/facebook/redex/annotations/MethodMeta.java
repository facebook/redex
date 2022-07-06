/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.annotations;

import static java.lang.annotation.RetentionPolicy.CLASS;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.Target;

/**
 * Add this annotation to a generated method that is potentially a dedup target.
 *
 * <p>The constant types and values are later on processed by Redex during class
 * merging.
 */
@Target({ElementType.METHOD})
@Retention(CLASS)
public @interface MethodMeta {

  /**
   * The types of the constants to be lifted encoded in a single string e.g., "IZS" represents int,
   * boolean and String.
   */
  public String constantTypes() default "";

  /**
   * The values of the constants to be lifted encoded in a single string delimited by '|' e.g.,
   * "42|false|foo". With the matching type encodeding shown above, the values translate to int 42,
   * boolean false and String "foo".
   */
  public String constantValues() default "";
}
