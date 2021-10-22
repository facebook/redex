/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


@UsesAppModule({"classes", "AppModule"})
public class AppModuleUsageOtherClass {
  @UsesAppModule({"AppModule"}) public int field;
  // Three stores violation 1; Two stores no violation
  public AppModuleUsageThirdClass third_class_field;
  AppModuleUsageOtherClass() {}
  public void doSomethingToMe() {}
  public static void otherMethod() {}
  public static void internalMethod() {}
  public static void otherMethod2() {}
  public static void otherMethod3() {
    AppModuleUsageClass.method3();
    // circular, shouldn't break test
  }
  // NOT an app module reference
  public static void selfReflection() {
    try {
      Class.forName("AppModuleUsageOtherClass").getDeclaredConstructors();
    } catch (ClassNotFoundException | SecurityException ignoreException) {
      /* okay to ignore in test*/
    }
  }
  // Three stores violation 2; Two stores no violation
  public void thirdMethod() { third_class_field.method(); }
}
