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

-optimizations !method/inlining/*


-keepattributes SourceFile,LineNumberTable,Signature

-printmapping
