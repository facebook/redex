-dontobfuscate
-dontshrink
-dontoptimize

-keep class com.facebook.redextest.OptimizeEnumSwitchMapTest { *; }
-keep class com.facebook.redextest.EnumA { *; }
-keep class com.facebook.redextest.EnumB { *; }
-keep class com.facebook.redextest.BigEnum { *; }
-keep class com.facebook.redextest.Foo { *; }

-keep class com.facebook.redextest.kt.OptimizeEnumSwitchMapTest { *; }
-keep class com.facebook.redextest.kt.OptimizeEnumSwitchMapTestKt { *; }

# Don't muck with test infra
-keep class org.fest.** { *; }
-keep class org.junit.** { *; }
-keep class junit.** { *; }
-keep class sun.misc.** { *; }
-keep class android.test.** { *; }
-keep class android.support.test.** { *; }
-keep class androidx.test.** { *; }
