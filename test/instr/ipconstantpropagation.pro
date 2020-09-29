-dontobfuscate
-dontoptimize
-dontshrink

-keepclassmembers class * {
  @org.junit.Test *;
}

-keepnames class redex.NoChangeNameIntf {
  *;
}
