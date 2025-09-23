-dontobfuscate
-dontshrink

-keep public class redex.ComposeUITestKt

-keepclassmembers public class redex.ComposeUITestKt {
  public static void HelloWorldText(androidx.compose.runtime.Composer, int);
}
