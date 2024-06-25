---
id: faq
title: FAQ
---

## I'm getting "Couldn't find zipalign. See README.md to resolve this." or other zipalign errors

`zipalign` is an optimization step that is bundled with the Android SDK.  You
need to tell redex where to find it.  For example, if you installed the SDK at
`/path/to/android/sdk`, try:
```
ANDROID_SDK=/path/to/android/sdk redex [... arguments ...]
```
You can alternatively add `zipalign` to your PATH, for example:
```
PATH=/path/to/android/sdk/build-tools/xx.y.zz:$PATH redex [... arguments ...]
```

Additionally, it's possible zipalign itself failed. In this case the stderr will be reported to aid debugging (for example, zipalign fails if you provide an output path that already exists).

## My app fails to install with `Failure [INSTALL_PARSE_FAILED_NO_CERTIFICATES]`

After you run redex, you will need to re-sign your app.  You can re-sign manually
using these instructions:
https://developer.android.com/tools/publishing/app-signing.html#signing-manually.

You can also tell redex to sign for you.  If you want to sign with the debug
key, you can simply do:

```
redex --sign [ ... arguments ...]
```

If you want to sign with your release key, you'll need to provide the
appropriate args:

```
--sign Sign the apk after optimizing it
-s [KEYSTORE], --keystore [KEYSTORE]
-a [KEYALIAS], --keyalias [KEYALIAS]
-p [KEYPASS], --keypass [KEYPASS]
```

## My App crashes with `MethodNotFoundException`, `ClassNotFoundException`, `NoSuchFieldException`, or something similar. How do I fix this?

Redex probably deleted or renamed it. Redex is quite aggressive about deleting
things it deems are unreachable. But, often Redex doesn't know about reflection
or other complex ways an entity could be reached.

Here's how you ensure Redex will not delete or rename something:

Annotate any class, method, or field you want to keep with `@DoNotStrip`, and
add this to your ProGuard config file (which should be passed to redex.py via
the `--proguard-config` flag):

```
# Do not strip classes annotated with @DoNotStrip
-keep @com.path.to.your.DoNotStrip class *
# Do not strip annotated fields or methods either
-keepclassmembers class * {
    @com.path.to.your.DoNotStrip *;
}
```

and define `DoNotStrip`:

```
package com.path.to.your;
public @interface DoNotStrip {}
```

## How does this compare to ProGuard?

ReDex is conceptually similar to ProGuard, in that both optimize bytecode.
ReDex, however, optimizes .dex bytecode, while ProGuard optimizes .class
bytecode before it is lowered to .dex.  Operating on .dex is sometimes an
advantage: you can consider the number of virtual registers used by a method
that is an inlining candidate, and you can control the layout of classes within
a dex file.  But ProGuard has some capabilities that ReDex does not (for
example, ReDex will not remove unused method parameters, which ProGuard does).

## How about DexGuard?

DexGuard operates on dex, but we haven't evaluated it at all since it's closed
source.  We don't use it at Facebook and we have no plans to start.
