# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

-dontobfuscate
-dontoptimize
-dontshrink

-keep,allowobfuscation @interface com.facebook.proguard.annotations.DoNotStrip
-keep @com.facebook.proguard.annotations.DoNotStrip class *
-keepclassmembers class ** { @com.facebook.proguard.annotations.DoNotStrip *; }
