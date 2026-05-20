# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

-dontobfuscate
-dontoptimize
-dontshrink

# Don't muck with test infra

-keep class org.junit.** { *; }
-keep class junit.** { *; }
-keep class sun.misc.** { *; }
-keep class android.test.** { *; }
-keep class android.support.test.** { *; }
-keep class androidx.test.** { *; }

# Keep @KeepForRedexTest

-keep @com.facebook.redex.test.instr.KeepForRedexTest class *
-keepclassmembers class * {
  @com.facebook.redex.test.instr.KeepForRedexTest *;
}


