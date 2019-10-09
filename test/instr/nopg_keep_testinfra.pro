-dontobfuscate
-dontoptimize
-dontshrink

# Don't muck with test infra

-keep class org.fest.** { *; }
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

-dontwarn org.fest.**
-dontwarn org.junit.**
-dontwarn junit.**
-dontwarn sun.misc.**
-dontwarn android.test.**
-dontwarn android.support.test.**
-dontwarn androidx.test.**
-dontwarn com.facebook.ultralight.**
-dontwarn android.content.**

-dontwarn org.xmlpull.v1.**
