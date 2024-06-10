/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This Java class is used to test the constant propagation and conditional
 * pruning optimization that needs to be written.
 * Currently, The test uses localDCE.
 */

package com.facebook.redextest;

import java.util.function.Supplier;

public class Dex038 {
  static final String staticStringField = "stringFromStaticField";
  static String instanceStringField = "stringFromInstanceField";

  public static void print(Supplier<String> str) {
    System.out.println(str.get());
  }

  public static String staticStringSupplier() {
    return staticStringField;
  }

  public String instanceStringSupplier() {
    return instanceStringField;
  }

  private String privateInstanceStringSupplier() {
    return instanceStringField;
  }

  public void run() {
    final String localString = "stringFromClosure";
    // invoke-instance
    print(this::instanceStringSupplier);
    // invoke-direct
    print(this::privateInstanceStringSupplier);
    // invoke-constructor
    print(String::new);
    // invoke-static
    print(Dex038::staticStringSupplier);
    // invoke-static
    print(() -> {
      return "stringFromLocal";
    });
    // invoke-static
    print(() -> {
      return localString;
    });
    // invoke-static
    print(() -> {
      return instanceStringField;
    });
  }

  public static void main(String[] args) {
    new Dex038().run();
  }
}
