/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest.annotation;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Test-only annotation for marking a method that should receive generated
 * invoke-static calls to __initStatics$*() methods.
 *
 * Uses a different package name from the production annotation to exercise
 * the orchestrator_annotation config option in ClinitBatchingPass.
 */
@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.METHOD)
public @interface GenerateStaticInitBatch {
}
