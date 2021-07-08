/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex.jni.example;

import android.app.Activity;
import com.facebook.soloader.DoNotOptimize;
import com.facebook.soloader.SoLoader;
import com.facebook.soloader.annotation.SoLoaderLibrary;

@SoLoaderLibrary("fbjniexample")
public class FBJNIExample {
  public FBJNIExample(Activity a) {
    SoLoader.init(a, false);
    SoLoader.loadLibrary("fbjniexample");
  }

  public int doThing() {
    int a = implementedRegisteredDeclaredUsed();
    return a;
  }

  public native int implementedRegisteredDeclaredUsed();

  public native int implementedRegisteredDeclared();
}
