-dontobfuscate
-dontshrink

-keepclassmembers class redex.RemoveNullcheckStringArgTestKt {
  public static void testMethods();
  public static void staticMethodWithNullCheck(java.lang.String);
}

-keepclassmembers class redex.TestClass {
  public void virtualMethodWithNullCheck(java.lang.String);
}

-keepclassmembers class redex.TestClassWithConstructor {
  public void <init>(int, java.lang.String);
  public void printInfo();
}

-keepclassmembers class redex.TestClassWithCompanion {
  public static void jvmStaticMethodWithNullCheck(java.lang.String);
}
