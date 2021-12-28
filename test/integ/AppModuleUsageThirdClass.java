/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */


@UsesAppModule({"AppModule"})
public class AppModuleUsageThirdClass {
  public AppModuleUsageOtherClass field;
  public static void method() { AppModuleUsageOtherClass.otherMethod(); }
}
