-keep class com.facebook.redex.test.proguard.Gamma

-keep class com.facebook.redex.test.proguard.Delta$B

-keep class com.facebook.redex.test.proguard.Delta$C
-keep class com.facebook.redex.test.proguard.Delta$C {
  *;
}

-keep class com.facebook.redex.test.proguard.Delta
-keep class com.facebook.redex.test.proguard.Delta {
  public <init>();
  public <init>(java.lang.String);
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
-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$J {
  public <init>(com.facebook.redex.test.proguard.Delta);
  public <init>(com.facebook.redex.test.proguard.Delta, java.lang.String);
  ** *_bear;
  public *** alpha?;
  public ** beta*;
  public **[] gamma*;
  public int omega(int, boolean, java.lang.String, char);
  public int omega(%);
  public int theta(...);
  public % zeta?();
}

# DoNotStrip Annotation
-keep @interface com.facebook.redex.test.proguard.DoNotStrip
-keep @com.facebook.redex.test.proguard.DoNotStrip class **
-keep @com.facebook.redex.test.proguard.DoNotStrip class ** {
  @com.facebook.redex.test.proguard.DoNotStrip *;
}
