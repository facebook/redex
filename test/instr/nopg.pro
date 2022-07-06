-dontobfuscate
-dontoptimize
-dontshrink

-keep,allowobfuscation @interface com.facebook.proguard.annotations.DoNotStrip
-keep @com.facebook.proguard.annotations.DoNotStrip class *
-keepclassmembers class ** { @com.facebook.proguard.annotations.DoNotStrip *; }
