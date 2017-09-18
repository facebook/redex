-keepnames class *

-keepclassmembers class * {*; }

-dontshrink

# Don't muck with test infra

-keep class org.fest.** { *; }
-keep class org.junit.** { *; }
-keep class junit.** { *; }
-keep class sun.misc.** { *; }
-keep class android.test.** { *; }
-keep class android.support.test.** { *; }

-optimizations !method/inlining/*

-dontwarn org.fest.**
-dontwarn org.junit.**
-dontwarn junit.**
-dontwarn sun.misc.**
-dontwarn android.test.**
-dontwarn android.support.test.**

-keepattributes SourceFile,LineNumberTable,Signature

-printmapping
