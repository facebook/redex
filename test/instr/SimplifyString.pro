-keepnames class *

-keepclassmembers class * {*; }

-dontshrink

# Don't muck with test infra

-keep class org.junit.** { *; }
-keep class junit.** { *; }
-keep class sun.misc.** { *; }
-keep class android.test.** { *; }
-keep class android.support.test.** { *; }
-keep class androidx.test.** { *; }

# Unit test will have -dontoptimize from somewhere, so this is technically
# has no effect.
-optimizations !method/inlining/*


-keepattributes SourceFile,LineNumberTable,Signature

-printmapping

