# Configuration for ProGuard matcher e2e tests.

-dontoptimize

-keep class com.facebook.redex.test.proguard.Gamma

-keep class com.facebook.redex.test.proguard.Delta$B

-keep class com.facebook.redex.test.proguard.Delta$C {
  *;
}

-keep class com.facebook.redex.test.proguard.Delta {
  public <init>();
  public <init>(java.lang.String);
  !public static <fields>;
}

-keep class com.facebook.redex.test.proguard.Delta$D {
  <fields>;
}

-keep class com.facebook.redex.test.proguard.Delta$E {
  <methods>;
}

-keep class com.facebook.redex.test.proguard.Delta$F {
  final <fields>;
}

-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$G {
  *;
}

-keep,allowobfuscation class com.facebook.redex.test.proguard.Delta$H {
  int wombat;
}

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
-keep @com.facebook.redex.test.proguard.DoNotStrip class ** {
  @com.facebook.redex.test.proguard.DoNotStrip *;
}

-keep class com.facebook.redex.test.proguard.Delta$L {
  public protected void alpha?();
  private void alpha0();
  void beta?();
  public protected private void gamma?();
}

# DontKillMe Annotation
-keep @interface com.facebook.redex.test.proguard.DontKillMe

-keep public !final class ** extends com.facebook.redex.test.proguard.Epsilon
-keep,allowobfuscation class ** extends com.facebook.redex.test.proguard.Delta$G
-keep class ** extends @com.facebook.redex.test.proguard.DoNotStrip com.facebook.redex.test.proguard.Delta$R?

-keep class ** extends android.graphics.Color
-keep class ** implements android.text.Editable

-keep class ** implements com.facebook.redex.test.proguard.Eta$T0

-keep class com.facebook.redex.test.proguard.Theta

-keep class com.facebook.redex.test.proguard.Eta$T?

-keep interface com.facebook.redex.test.proguard.Iota$MySerializable
-keepclassmembers public class ** implements com.facebook.redex.test.proguard.Iota$MySerializable {
   public int encode(int);
   public int decode(int);
}
-keep class com.facebook.redex.test.proguard.Iota$Alpha
-keep class com.facebook.redex.test.proguard.Iota$SomeOther {
  *;
}

-keepclasseswithmembers class * {
  void red();
  void green?();
}

-keepclasseswithmembers class * {
  com.facebook.redex.test.proguard.Delta$VT *;
}

-keepnames class com.facebook.redex.test.proguard.Delta$W {
  *;
}

-keepnames class ** implements com.facebook.redex.test.proguard.Delta$S3 {
   public int alpha;
   public int beta;
}

-keep class com.facebook.redex.test.proguard.Delta$T2

-keep class com.facebook.redex.test.proguard.Delta$U {
  void mutator();
}
-assumenosideeffects class com.facebook.redex.test.proguard.Delta$U {
  void logger();
}

-keepclasseswithmembers class * {
    public <init>(com.facebook.redex.test.proguard.Delta$X);
}

-keepclasseswithmembernames class * {
    native <methods>;
}

-keepclasseswithmembers class com.facebook.redex.test.proguard.Delta$E7 {
  int crab;
  int seahorse;
  int shark();
  int tuna?();
}

# view AndroidManifest.xml #generated:6
-keep class android.support.test.runner.AndroidJUnitRunner {
    <init>(...);
}
-keep class androidx.test.runner.AndroidJUnitRunner {
    <init>(...);
}

# view AndroidManifest.xml #generated:13
-keep class com.facebook.redex.test.proguard.ProguardTest {
    <init>(...);
}
-dontwarn android.content.**

-dontwarn org.xmlpull.v1.**
