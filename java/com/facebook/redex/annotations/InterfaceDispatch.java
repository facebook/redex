// Copyright 2004-present Facebook. All Rights Reserved.

package com.facebook.redex.annotations;

import static java.lang.annotation.RetentionPolicy.CLASS;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.Target;

/**
 * Redex might add this annotation to generated interface dispatch methods. It is later on picked up
 * for another post processing steps in Redex to further optimize these dispatch methods.
 */
@Target({ElementType.METHOD})
@Retention(CLASS)
public @interface InterfaceDispatch {}
