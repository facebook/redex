
-keep class Foo {
public final void main()        ;
}

-keep,allowobfuscation @interface com.facebook.proguard.annotations.DoNotStrip
-keep @com.facebook.proguard.annotations.DoNotStrip class *
-keepclassmembers class ** { @com.facebook.proguard.annotations.DoNotStrip *; }

-allowaccessmodification
-dontobfuscaterintmapping
