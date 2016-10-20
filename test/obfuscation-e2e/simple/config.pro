# Allow the fields of class Alpha to be renamed.
-keep class com.facebook.redex.test.proguard.Alpha
-keep,allowobfuscation class com.facebook.redex.test.proguard.Alpha {
  *;
}

-keep class com.facebook.redex.test.proguard.Beta
-keep class com.facebook.redex.test.proguard.Beta {
  *;
}

-keep class com.facebook.redex.test.proguard.Hello
-keep,allowobfuscation class com.facebook.redex.test.proguard.Hello {
  *;
}

-keep class com.facebook.redex.test.proguard.World
-keep,allowobfuscation class com.facebook.redex.test.proguard.World {
  *;
}

-keep class com.facebook.redex.test.proguard.All
-keep,allowobfuscation class com.facebook.redex.test.proguard.All {
  *;
}
