/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.annotations;

import static java.lang.annotation.RetentionPolicy.CLASS;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.Target;

/** Declaration for a class hierarchy that has its own concept of a type tag identity system. */
@Target({ElementType.TYPE})
@Retention(CLASS)
public @interface ModelIdentity {
  int typeTag();
}
