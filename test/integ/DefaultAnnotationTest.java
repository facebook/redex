/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This Java class is used to test the parsing of default annotation values
 */

package com.facebook.redextest;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.ElementType;
import java.lang.annotation.Target;

@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.METHOD)
@interface ExampleAnnotation {
  String strVal() default "defaultStrValue";
  int intVal() default 42;
  boolean booleanVal() default true;
}

class Annotation {

  @ExampleAnnotation
  public static void foo() {
    return;
  }

  @ExampleAnnotation(strVal = "overriddenStrValue", intVal = 100, booleanVal=false)
  public static void bar() {
    return;
  }
}

public class DefaultAnnotationTest {
  public static void main(String[] args) {
    Annotation.foo();
    Annotation.bar();
  }
}
