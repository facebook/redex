---
id: synth
title: Redex Synth Pass Example
---

> The code and artifacts for this example are on [GitHub](https://github.com/facebook/redex/tree/master/examples/Synth).

The synth optimizations attempt to remove synthetic methods and wrappers. This
improves performance by making access to field values faster and it also
reduces code size because the definition and invocation of synthetic methods
is eliminated.

## Removing synthetic methods for accessing static fields

This directory contains an Android Studio 1.5 project that illustrates how a
wrapper synthetic method is removed by Redex. The contrived example is
a simple Android application which makes use of this class:

```java
package com.facebook.redex.examples.synth;

public class Alpha {

    private static int alpha;

    public Alpha(int initialValue) {
        alpha = initialValue;
    }

    public class Beta {
        public int doubleAlpha() {
            return 2 * alpha;
        }
    }
}
```

The key thing to note here is that there is an inner class `Beta` which
has a method `doubleAlpha` which accesses a private static field `alpha` of
its outer class `Alpha`. A dump of the Dex bytecode for the `Alpha` class
confirms that the `alpha` field is `private` and `static`.

```
  Static fields     -
    #0              : (in Lcom/facebook/redex/examples/synth/Alpha;)
      name          : 'alpha'
      type          : 'I'
      access        : 0x000a (PRIVATE STATIC)
```

Java compilers will implement accesses] to this field from inner classes
by generating a synthetic getter method to allow the inner class to access
the private field of the outer
class. We can see the automatically generated synthetic method `access$000`
if we examine the Dex bytecode for the `Alpha` class:

```
Class #597            -
  Class descriptor  : 'Lcom/facebook/redex/examples/synth/Alpha;'
...
    #1              : (in Lcom/facebook/redex/examples/synth/Alpha;)
      name          : 'access$000'
      type          : '()I'
      access        : 0x1008 (STATIC SYNTHETIC)
      code          -
      registers     : 1
      ins           : 0
      outs          : 0
      insns size    : 3 16-bit code units
065f14:                                        |[065f14] com.facebook.redex.examples.synth.Alpha.access$000:()I
065f24: 6000 070b                              |0000: sget v0, Lcom/facebook/redex/examples/synth/Alpha;.alpha:I // field@0b07
065f28: 0f00                                   |0002: return v0
```

This generated synthetic method can be accessed by the inner class
which keeps the JVM happy. All it does is to read the state field
value using an `sget` instruction and return the read value.
This synthetic wrapper is used in the implementation of `doubleAlpha`:

```
  Virtual methods   -
    #0              : (in Lcom/facebook/redex/examples/synth/Alpha$Beta;)
      name          : 'doubleAlpha'
...
065ed8:                                        |[065ed8] com.facebook.redex.examples.synth.Alpha.Beta.doubleAlpha:()I
065ee8: 7100 1118 0000                         |0000: invoke-static {}, Lcom/facebook/redex/examples/synth/Alpha;.access$000:()I // method@1811
065eee: 0a00                                   |0003: move-result v0
065ef0: da00 0002                              |0004: mul-int/lit8 v0, v0, #int 2 // #02
065ef4: 0f00                                   |0006: return v0
```

For the `doubleAlpha` method in the inner class `Beta` to access the
private static field of the enclosing class an `invoke-static` call is made
to the synthetic wrapper method `access$000`.

Here is an example of the `doubleAlpha` method being used from a simple
Android application:

```java
package com.facebook.redex.examples.synth;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.widget.TextView;

public class MainActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        TextView textView = (TextView) findViewById(R.id.message);
        textView.setText("Redex Synth Example\n");

        Alpha a = new Alpha(12);
        Alpha.Beta b = a.new Beta();
        textView.append("Double Alpha(12) = " + b.doubleAlpha() + "\n");
    }
}
```

If we examined the Dex bytecode for the call to `doubleAlpha` we would
see something like this which shows a virtual call to the `doubleAlpha` method:

```
  Direct methods    -
    #0              : (in Lcom/facebook/redex/examples/synth/MainActivity;)
      name          : 'onCreate'
...
065fb6: 6e10 0f18 0100                         |0031: invoke-virtual {v1}, Lcom/facebook/redex/examples/synth/Alpha$Beta;.doubleAlpha:()I // method@180f
...
```

After running Redex the synthetic wrapper method will be removed and
the code for accessing the `alpha` static field will be inlined into
the code for the main activity:

```
  Direct methods    -
    #0              : (in Lcom/facebook/redex/examples/synth/MainActivity;)
      name          : 'onCreate'
...
07f246: 6005 4d06                              |0031: sget v5, Lcom/facebook/redex/examples/synth/Alpha;.alpha:I // field@064d
07f24a: da05 0502                              |0033: mul-int/lit8 v5, v5, #int 2 // #02
...

```

To make the access of the `alpha` field legal for the Dalvik VM we have
mutated the access permissions for this field:

```
  Static fields     -
    #0              : (in Lcom/facebook/redex/examples/synth/Alpha;)
      name          : 'alpha'
      type          : 'I'
      access        : 0x0009 (PUBLIC STATIC)

```

Similar optimizations exist for other synthetic wrapper scenarios e.g.
for instance fields.

## Example Code
The project in the `synth-example` directory can be opened with Android Studio 1.5 and
contains the sample here that illustrates the removal of synthetic
wrappers for static private fields. 

The `Makefile` in this directory can be used once you have build a signed:
APK (`Build : Generate Signed APK...`) to produce the following items:
* `synth-example-release-redex.apk`: A Redex optimized version of the original APK.
* `classes.dump`: A dump of the Dex bytecode for the input APK `synth-example-release.apk`.
* `classes-redex.dump`: A dump of the Redex optimizd APK `synth-example-release-redex.apk`.

The environment variable `ANDROID_TOOLS` should be set to the location
of your Android SDK tools directory.

To produce these items:

```
$ make clean all
```
