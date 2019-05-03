-dontobfuscate
-dontshrink
-dontoptimize

-keep class com.facebook.redex.test.instr.UsingNoEscapeBuilder {
  *;
}

# Don't muck with test infra

-keep class org.fest.** { *; }
-keep class org.junit.** { *; }
-keep class junit.** { *; }
-keep class sun.misc.** { *; }
-keep class android.test.** { *; }
-keep class android.support.test.** { *; }
-keep class androidx.test.** { *; }

-dontwarn org.fest.**
-dontwarn org.junit.**
-dontwarn junit.**
-dontwarn sun.misc.**
-dontwarn android.test.**
-dontwarn android.support.test.**
-dontwarn androidx.test.**
-dontwarn android.content.**

-dontwarn org.xmlpull.v1.**
