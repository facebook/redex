-keep class com.facebook.redex.test.proguard.Gamma

-keep class com.facebook.redex.test.proguard.Delta$B

-keep class com.facebook.redex.test.proguard.Delta$C
-keep class com.facebook.redex.test.proguard.Delta$C {
  *;
}

-keep class com.facebook.redex.test.proguard.Delta
-keep class com.facebook.redex.test.proguard.Delta {
  public <init>();
  public <init>(int);
  !public static <fields>;
}

-keep class com.facebook.redex.test.proguard.Delta$D
-keep class com.facebook.redex.test.proguard.Delta$D {
  <fields>;
}

-keep class com.facebook.redex.test.proguard.Delta$E
-keep class com.facebook.redex.test.proguard.Delta$E {
  <methods>;
}

-keep class com.facebook.redex.test.proguard.Delta$F
-keep class com.facebook.redex.test.proguard.Delta$F {
  final <fields>;
}

-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$G
-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$G {
  *;
}

-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$H
-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$H {
  int wombat;
}

-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$I
-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$I {
  int wombat*;
}

-keep class com.facebook.redex.test.proguard.Delta$J
-keep class com.facebook.redex.test.proguard.Delta$J {
  ** *_bear;
  public *** alpha?;
  public ** beta*;
}
