# Allow the fields of class Alpha to be renamed.
-keep class com.facebook.redex.test.proguard.TheSuper
-keep,allowobfuscation class com.facebook.redex.test.proguard.TheSuper {
  *;
}

-keep class com.facebook.redex.test.proguard.ImplOne
-keep,allowobfuscation class com.facebook.redex.test.proguard.ImplOne {
  *;
}

-keep class com.facebook.redex.test.proguard.Sub
-keep,allowobfuscation class com.facebook.redex.test.proguard.Sub {
  *;
}

-keep class com.facebook.redex.test.proguard.SubImpl
-keep,allowobfuscation class com.facebook.redex.test.proguard.SubImpl {
  *;
}

-keep class com.facebook.redex.test.proguard.SubSub
-keep,allowobfuscation class com.facebook.redex.test.proguard.SubSub {
  *;
}

-dontwarn android.content.**

-dontwarn org.xmlpull.v1.**
