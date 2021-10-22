/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

@UsesAppModule({"AppModule"})
public class UsesAppModuleAnnotated {
  @UsesAppModule({"AppModule"}) private AppModuleUsageOtherClass field;

  @UsesAppModule({"AppModule", "classes"})
  public AppModuleUsageOtherClass field2;

  public static void method0() { AppModuleUsageOtherClass.otherMethod3(); }

  @UsesAppModule({"AppModule"})
  public static void method1() {
    AppModuleUsageOtherClass.otherMethod();
  }

  @UsesAppModule({"AppModule", "classes"})
  public void method2() {
    field = new AppModuleUsageOtherClass();
    field.doSomethingToMe();
  }
}
