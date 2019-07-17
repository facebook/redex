-dontobfuscate
-dontoptimize
-dontshrink

-keep class com.facebook.redextest.EnumTestRemoveGenerated { *; }

-dontwarn org.fest.**
-dontwarn org.junit.**
-dontwarn junit.**
-dontwarn sun.misc.**
-dontwarn android.test.**
-dontwarn android.support.test.**
-dontwarn com.facebook.ultralight.**
-dontwarn com.facebook.graphservice.**
-dontwarn androidx.test.**

-dontwarn android.content.**

-dontwarn org.xmlpull.v1.**
