# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

-dontobfuscate
-dontshrink

-keep public class redex.ComposeUITestKt

-keepclassmembers public class redex.ComposeUITestKt {
  public static void HelloWorldText(androidx.compose.runtime.Composer, int);
}
