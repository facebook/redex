# Allow the fields of class Alpha to be renamed.

-keep class com.facebook.redex.test.proguard.Intf1
-keep,allowobfuscation class com.facebook.redex.test.proguard.Intf1 {
  *;
}

-keep class com.facebook.redex.test.proguard.IntfParent
-keep,allowobfuscation class com.facebook.redex.test.proguard.IntfParent {
  *;
}

-keep class com.facebook.redex.test.proguard.Intf2
-keep,allowobfuscation class com.facebook.redex.test.proguard.Intf2 {
  *;
}

-keep class com.facebook.redex.test.proguard.IntfSub
-keep,allowobfuscation class com.facebook.redex.test.proguard.IntfSub {
  *;
}

-keep class com.facebook.redex.test.proguard.Foo
-keep,allowobfuscation class com.facebook.redex.test.proguard.Foo {
  *;
}

-keep class com.facebook.redex.test.proguard.AbstractClass
-keep,allowobfuscation class com.facebook.redex.test.proguard.AbstractClass {
  *;
}

-keep class com.facebook.redex.test.proguard.Bar
-keep,allowobfuscation class com.facebook.redex.test.proguard.Bar {
  *;
}

-keep class com.facebook.redex.test.proguard.Baz
-keep,allowobfuscation class com.facebook.redex.test.proguard.Baz {
  *;
}

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

-dontwarn android.content.**

-dontwarn org.xmlpull.v1.**
