-dontobfuscate
-dontshrink

-optimizations !class/*, !field/*, !method/*

# Keep @KeepForRedexTest

-keep @com.facebook.redex.test.instr.KeepForRedexTest class *
-keepclassmembers class * {
  @com.facebook.redex.test.instr.KeepForRedexTest *;
}

-keep class com.facebook.redexinline.ForceInline { *; }

# Keep classes with @Test or @Before methods

-keepclasseswithmembers class * {
  @org.junit.Test <methods>;
}

-keepclasseswithmembers class * {
  @org.junit.Before <methods>;
}

-keepclassmembers class * {
  <init>(...);
}

# Don't muck with test infra

-keep class org.junit.** { *; }
-keep class junit.** { *; }
-keep class sun.misc.** { *; }
-keep class android.test.** { *; }
-keep class android.support.test.** { *; }
-keep class androidx.test.** { *; }

-optimizations !method/inlining/*
